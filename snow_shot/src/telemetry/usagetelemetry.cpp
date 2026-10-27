#include "snow_shot/telemetry/usagetelemetry.h"

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/telemetry/usageevents.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace snow_shot::telemetry {
namespace {
// Fixed budgets. Every number here is a ceiling on resource use, not a target, so that the
// cost of telemetry is the same for a user who takes one screenshot a week and one who works
// in the app all day.
constexpr qsizetype kRingCapacity = 512; // 512 * 24 B ~= 12 KiB
constexpr qsizetype kMaxSamplesPerRequest = 100;
constexpr qsizetype kMaxBodyBytes = 64 * 1024;
constexpr qint64 kRetryBaseMs = 60 * 1000;
constexpr qint64 kRetryMaxMs = 30 * 60 * 1000;
constexpr int kRetryMaxShift = 5;
constexpr int kShutdownBudgetMs = 500;

struct Ring {
    std::mutex mutex;
    std::array<UsageSample, static_cast<std::size_t>(kRingCapacity)> samples{};
    qsizetype head = 0; // number of buffered samples; new ones land at [head]
    quint64 dropped = 0;
};

// Function-local static: constructed on first use, thread-safe, and never touched by static
// destruction ordering problems because the recording thread may still reach it during exit.
Ring& ring() {
    static Ring instance;
    return instance;
}

std::atomic<bool> tapInstalled{false};
std::atomic<bool> stopped{false};

void recordSample(const UsageSample& sample) noexcept {
    Ring& buffer = ring();
    std::lock_guard<std::mutex> lock(buffer.mutex);
    if (buffer.head >= kRingCapacity) {
        // Drop the newest rather than the oldest: keeping the tail in time order means the
        // batches that do get sent are internally consistent.
        ++buffer.dropped;
        return;
    }
    buffer.samples[buffer.head++] = sample;
}

qsizetype takeSamples(std::span<UsageSample> out) noexcept {
    Ring& buffer = ring();
    std::lock_guard<std::mutex> lock(buffer.mutex);
    const auto count = std::min<qsizetype>(buffer.head, static_cast<qsizetype>(out.size()));
    std::copy_n(buffer.samples.begin(), count, out.begin());
    std::move(buffer.samples.begin() + count, buffer.samples.begin() + buffer.head,
              buffer.samples.begin());
    buffer.head -= count;
    return count;
}

void usageTapSink(void* context, QtMsgType level, const QString& category, const QString& event,
                  const QJsonObject& fields) noexcept {
    static_cast<void>(context);
    try {
        // Stamp only after the cheap catalogue lookup succeeds: an uncounted event must not pay
        // for a clock read.
        auto sample = usageSampleForDiagnosticEvent(level, category, event, fields, 0);
        if (!sample)
            return;
        sample->epochMs = QDateTime::currentMSecsSinceEpoch();
        recordSample(*sample);
    } catch (...) {
        // A noexcept sink that threw would terminate the process, so nothing escapes here.
    }
}

QString defaultUserId() {
#ifdef Q_OS_WIN
    const QString profile = qEnvironmentVariable("USERPROFILE");
    if (!profile.isEmpty()) {
        const QString name = QFileInfo(QDir::toNativeSeparators(profile)).fileName();
        if (!name.isEmpty())
            return name;
    }
#endif
    const QString name = QFileInfo(QDir::homePath()).fileName();
    return name.isEmpty() ? QStringLiteral("unknown") : name;
}

QString defaultAppName() {
    return QStringLiteral("SnowShot");
}

// Mirrors SnowShotApiClient's behaviour: honour the system proxy only when the user asked for
// it, and never weaken certificate checks.
class SystemNetworkProxyFactory final : public QNetworkProxyFactory {
  public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
        return systemProxyForQuery(query);
    }
};

class UsageTelemetry final : public QObject {
  public:
    explicit UsageTelemetry(Options options)
        : m_options(std::move(options)), m_useSystemProxy(m_options.useSystemProxy),
          m_timer(new QTimer(this)) {
        m_timer->setSingleShot(true);
        QObject::connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    }

    void begin() {
        recordSample({QDateTime::currentMSecsSinceEpoch(), UsageEvent::SessionStart,
                      UsageOutcome::Unknown, UsageFeature::None});
        m_timer->start(static_cast<int>(m_options.firstFlushDelay.count()));
    }

    void setUseSystemProxy(bool enabled) {
        m_useSystemProxy = enabled;
        if (m_manager != nullptr)
            applyProxy();
    }

    // The only sender: there is deliberately no per-sample flush trigger, so the request rate is
    // a property of the clock and not of how much the user does.
    void tick() {
        flush();
        m_timer->start(static_cast<int>(m_options.flushInterval.count()));
    }

    void shutdown() {
        m_timer->stop();
        if (!m_reply.isNull()) {
            // An in-flight batch cannot be confirmed any more; drop it instead of leaving a
            // dangling reply behind a destroyed network manager.
            m_reply->disconnect();
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        // Record the shutdown before the final attempt so the tail window is not lost.
        recordSample({QDateTime::currentMSecsSinceEpoch(), UsageEvent::SessionCleanShutdown,
                      UsageOutcome::Unknown, UsageFeature::None});
        if (QCoreApplication::instance() != nullptr)
            deliverRemaining();
        if (m_manager != nullptr) {
            // Deleted outright rather than deferred: aboutToQuit runs when no further deferred
            // deletes are guaranteed to be processed.
            delete m_manager;
            m_manager = nullptr;
        }
        m_proxyFactory.reset();
    }

  private:
    void applyProxy() {
        if (m_useSystemProxy) {
            if (m_proxyFactory == nullptr) {
                m_proxyFactory = std::make_unique<SystemNetworkProxyFactory>();
                m_manager->setProxyFactory(m_proxyFactory.get());
            }
            return;
        }
        m_manager->setProxy(QNetworkProxy::NoProxy);
        if (m_proxyFactory != nullptr) {
            m_manager->setProxyFactory(nullptr);
            m_proxyFactory.reset();
        }
    }

    QNetworkAccessManager* manager() {
        if (m_manager == nullptr) {
            m_manager = new QNetworkAccessManager(this);
            m_manager->setTransferTimeout(static_cast<int>(m_options.transferTimeout.count()));
            applyProxy();
        }
        return m_manager;
    }

    bool buildRequest(QNetworkRequest* request) const {
        if (!m_options.endpoint.isValid() || m_options.endpoint.isEmpty())
            return false;
        request->setUrl(m_options.endpoint);
        request->setHeader(QNetworkRequest::ContentTypeHeader,
                           QStringLiteral("application/vnd.serilog.clef"));
        request->setRawHeader("Accept", "application/json");
        return true;
    }

    QByteArray encodeSample(const UsageSample& sample) const {
        const QDateTime moment = QDateTime::fromMSecsSinceEpoch(sample.epochMs);
        QJsonObject record;
        record.insert(QStringLiteral("@t"), moment.toUTC().toString(Qt::ISODateWithMs));
        // The template keeps Seq's own rendering readable while EventName stays a dimension.
        record.insert(QStringLiteral("@mt"), QStringLiteral("SnowShot usage {EventName}"));
        record.insert(QStringLiteral("@l"), QStringLiteral("Information"));
        record.insert(QStringLiteral("AppName"), m_options.appName);
        record.insert(QStringLiteral("UserId"), m_options.userId);
        record.insert(QStringLiteral("Date"), moment.date().toString(Qt::ISODate));
        record.insert(QStringLiteral("EventName"), usageEventName(sample.event));
        record.insert(QStringLiteral("Count"), 1);
        if (!m_options.appVersion.isEmpty())
            record.insert(QStringLiteral("AppVersion"), m_options.appVersion);
        if (!m_options.sessionId.isEmpty())
            record.insert(QStringLiteral("SessionId"), m_options.sessionId);
        const QString outcome = usageOutcomeName(sample.outcome);
        if (!outcome.isEmpty())
            record.insert(QStringLiteral("Outcome"), outcome);
        const QString feature = usageFeatureName(sample.feature);
        if (!feature.isEmpty())
            record.insert(QStringLiteral("Feature"), feature);
        return QJsonDocument(record).toJson(QJsonDocument::Compact);
    }

    // One CLEF record per line, no trailing newline. A batch is cut on a whole-record boundary
    // so the body never ends mid-JSON, and the size ceiling is checked before appending.
    QByteArray encodeBatch(std::span<const UsageSample> samples) const {
        QByteArray body;
        const qsizetype estimated = static_cast<qsizetype>(samples.size()) * 256;
        body.reserve(std::min(kMaxBodyBytes, estimated));
        for (const UsageSample& sample : samples) {
            const QByteArray line = encodeSample(sample);
            const qsizetype separator = body.isEmpty() ? 0 : 1;
            if (!body.isEmpty() && body.size() + separator + line.size() > kMaxBodyBytes)
                break;
            if (separator > 0)
                body.append('\n');
            body.append(line);
        }
        return body;
    }

    void noteFailure(const QString& reason) {
        ++m_failures;
        const int shift = std::min(m_failures - 1, kRetryMaxShift);
        const qint64 delay = std::min(kRetryBaseMs << shift, kRetryMaxMs);
        m_nextAttemptMs = QDateTime::currentMSecsSinceEpoch() + delay;
        if (m_failing)
            return;
        m_failing = true;
        // Logged only on the healthy -> failing edge so a permanently unreachable endpoint
        // cannot flood the log file. The event name is absent from the usage catalogue, so it
        // cannot feed back into the uploader.
        diagnostics::logEvent(QStringLiteral("snow_shot.telemetry"),
                              QStringLiteral("telemetry.flush_failed"),
                              {{QStringLiteral("reason"), reason}}, QtWarningMsg);
    }

    void noteSuccess() {
        m_failures = 0;
        m_nextAttemptMs = 0;
        if (!m_failing)
            return;
        m_failing = false;
        diagnostics::logEvent(QStringLiteral("snow_shot.telemetry"),
                              QStringLiteral("telemetry.flush_recovered"));
    }

    void flush() {
        if (!m_reply.isNull())
            return; // single-flight: the pending batch keeps the queue for the next window
        if (QDateTime::currentMSecsSinceEpoch() < m_nextAttemptMs)
            return;

        std::array<UsageSample, kMaxSamplesPerRequest> samples;
        const qsizetype count = takeSamples(samples);
        if (count <= 0)
            return;

        QNetworkRequest request;
        if (!buildRequest(&request))
            return;
        const std::span<const UsageSample> batch(samples.data(), static_cast<std::size_t>(count));
        const QByteArray body = encodeBatch(batch);
        if (body.isEmpty())
            return;

        QNetworkReply* reply = manager()->post(request, body);
        m_reply = reply;
        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            if (m_reply.data() == reply)
                m_reply = nullptr;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
                noteSuccess();
            else if (status >= 400 && status < 500 && status != 408 && status != 429)
                // A rejected payload will be rejected again; do not retry it, but keep the
                // backoff so a misconfigured client does not hammer the collector either.
                noteFailure(QStringLiteral("rejected_%1").arg(status));
            else
                noteFailure(reply->errorString().isEmpty() ? QStringLiteral("status_%1").arg(status)
                                                           : reply->errorString());
        });
    }

    // One bounded attempt on the way out. aboutToQuit still has a live event loop, so a short
    // nested loop is safe; a deadline makes sure a hung endpoint cannot hold the process open.
    void deliverRemaining() {
        std::array<UsageSample, kMaxSamplesPerRequest> samples;
        const qsizetype count = takeSamples(samples);
        if (count <= 0)
            return;
        QNetworkRequest request;
        if (!buildRequest(&request))
            return;
        const QByteArray body = encodeBatch(
            std::span<const UsageSample>(samples.data(), static_cast<std::size_t>(count)));
        if (body.isEmpty())
            return;

        QNetworkReply* reply = manager()->post(request, body);
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        deadline.start(kShutdownBudgetMs);
        loop.exec();
        reply->disconnect();
        reply->abort();
        reply->deleteLater();
    }

    Options m_options;
    QTimer* m_timer = nullptr;
    QNetworkAccessManager* m_manager = nullptr;
    std::unique_ptr<QNetworkProxyFactory> m_proxyFactory;
    QPointer<QNetworkReply> m_reply;
    bool m_useSystemProxy = false;
    bool m_failing = false;
    int m_failures = 0;
    qint64 m_nextAttemptMs = 0;
};

std::unique_ptr<UsageTelemetry>& instance() {
    static std::unique_ptr<UsageTelemetry> holder;
    return holder;
}
} // namespace

void installDiagnosticTap() noexcept {
    if (tapInstalled.exchange(true))
        return;
    diagnostics::setEventTap({&usageTapSink, nullptr});
}

void uninstallDiagnosticTap() noexcept {
    if (!tapInstalled.exchange(false))
        return;
    diagnostics::setEventTap({});
}

quint64 droppedSampleCount() noexcept {
    Ring& buffer = ring();
    std::lock_guard<std::mutex> lock(buffer.mutex);
    return buffer.dropped;
}

bool start(Options options) {
    if (instance() != nullptr)
        return true;
    if (!options.enabled)
        return false;

    if (options.userId.isEmpty())
        options.userId = defaultUserId();
    if (options.appName.isEmpty())
        options.appName = defaultAppName();

    // A fresh start is allowed after a previous stop() so tests can exercise the whole
    // lifecycle in one process; the application never does this.
    stopped.store(false);

    // Installed first so a failure below still leaves the catalogue warm for a later start().
    installDiagnosticTap();

    auto service = std::make_unique<UsageTelemetry>(std::move(options));
    service->begin();
    instance() = std::move(service);
    return true;
}

void stop() noexcept {
    if (stopped.exchange(true))
        return;
    uninstallDiagnosticTap();
    if (auto& service = instance(); service != nullptr) {
        // stop() is noexcept but the shutdown path allocates; a failure here must not become a
        // std::terminate during application exit.
        try {
            service->shutdown();
        } catch (...) {
        }
        service.reset();
    }
}

void setUseSystemProxy(bool enabled) noexcept {
    if (auto& service = instance(); service != nullptr)
        service->setUseSystemProxy(enabled);
}

} // namespace snow_shot::telemetry

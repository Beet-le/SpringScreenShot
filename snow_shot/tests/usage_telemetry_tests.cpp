#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/telemetry/usageevents.h"
#include "snow_shot/telemetry/usagetelemetry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>

using namespace snow_shot::telemetry;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void spin(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) {
    for (int waited = 0; waited < timeoutMs; waited += 5) {
        if (done())
            return true;
        spin(5);
    }
    return done();
}

QByteArray bodyOf(const QByteArray& request) {
    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
    return headerEnd < 0 ? QByteArray() : request.mid(headerEnd + 4);
}

QList<QByteArray> linesOf(const QByteArray& body) {
    QList<QByteArray> lines;
    for (const QByteArray& line : body.split('\n')) {
        if (!line.trimmed().isEmpty())
            lines.append(line);
    }
    return lines;
}

// A loopback stand-in for the Seq collector. It answers every complete request so the sender's
// single-flight slot is released by the real QNetworkReply code path, not by a shortcut.
class FakeSeqServer {
  public:
    FakeSeqServer() {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    qsizetype contentLength = 0;
                    for (const QByteArray& line : buffer->left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + contentLength)
                        return;
                    if (requests.size() < limit)
                        requests.append(buffer->left(headerEnd + 4 + contentLength));
                    const QByteArray payload = status < 300 ? "Created" : "Rejected";
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " Result\r\n"
                                  "Content-Type: text/plain\r\n"
                                  "Content-Length: " +
                                  QByteArray::number(payload.size()) +
                                  "\r\nConnection: close\r\n\r\n" + payload);
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost);
    }

    QUrl endpoint(const QString& path = QStringLiteral("/seq/api/events/raw?clef")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

    QList<QByteArray> requests;
    qsizetype limit = 1024;
    int status = 201;

  private:
    QTcpServer m_server;
};

Options fastOptions(const QUrl& endpoint) {
    Options options;
    options.endpoint = endpoint;
    options.userId = QStringLiteral("unit-test");
    options.appName = QStringLiteral("SnowShot");
    options.appVersion = QStringLiteral("0.0.0-test");
    options.sessionId = QStringLiteral("session-1");
    options.firstFlushDelay = std::chrono::milliseconds(20);
    options.flushInterval = std::chrono::milliseconds(20);
    options.transferTimeout = std::chrono::milliseconds(2000);
    return options;
}

void recordUsage(const QString& event, const QJsonObject& fields = {}) {
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.test"), event, fields);
}

// Every catalogued event needs the field its sample depends on, otherwise the mapping test
// would reject it for the wrong reason.
QJsonObject requiredFieldsFor(const QString& event) {
    if (event.startsWith(QStringLiteral("annotation.")))
        return {{QStringLiteral("tool"), QStringLiteral("arrow")}};
    return {{QStringLiteral("request_kind"), QStringLiteral("translation")}};
}

void catalogueMapsDiagnosticsEvents() {
    const QString category = QStringLiteral("snow_shot.test");
    for (const QString& name : trackedDiagnosticEvents()) {
        const auto sample = usageSampleForDiagnosticEvent(QtInfoMsg, category, name,
                                                          requiredFieldsFor(name), 7);
        require(sample.has_value(), "every catalogued event maps to a sample");
        require(sample->event != UsageEvent::Count, "the catalogue resolves a concrete event");
        require(sample->event != UsageEvent::SessionStart &&
                    sample->event != UsageEvent::SessionCleanShutdown,
                "session boundaries are synthesised by the collector, not mapped");
        require(sample->epochMs == 7, "the caller owns the timestamp");
        require(!usageEventName(sample->event).isEmpty(), "every event carries a stable name");
        require(!name.isEmpty(), "the catalogue exposes its diagnostics key");
    }

    require(!usageSampleForDiagnosticEvent(QtInfoMsg, category, QStringLiteral("qt.message"), {}, 0)
                 .has_value(),
            "Qt message records are never usage");
    require(!usageSampleForDiagnosticEvent(QtDebugMsg, category, QStringLiteral("capture.finished"),
                                           {}, 0)
                 .has_value(),
            "debug records are never usage");
    require(
        !usageSampleForDiagnosticEvent(QtInfoMsg, category, QStringLiteral("unknown.event"), {}, 0)
             .has_value(),
        "uncatalogued events are ignored");
    require(!usageSampleForDiagnosticEvent(QtInfoMsg, QStringLiteral("qt"),
                                           QStringLiteral("capture.finished"), {}, 0)
                 .has_value(),
            "records from other subsystems are ignored");

    const auto outcome = usageSampleForDiagnosticEvent(
        QtInfoMsg, category, QStringLiteral("capture.finished"),
        {{QStringLiteral("outcome"), QStringLiteral("cancelled")}}, 0);
    require(outcome.has_value() && outcome->outcome == UsageOutcome::Cancelled,
            "outcome is read from its field");

    const auto translated = usageSampleForDiagnosticEvent(
        QtInfoMsg, category, QStringLiteral("request.finished"),
        {{QStringLiteral("request_kind"), QStringLiteral("translation")},
         {QStringLiteral("outcome"), QStringLiteral("succeeded")}},
        0);
    require(translated.has_value() && translated->feature == UsageFeature::Translation,
            "translation requests carry their feature");
    require(usageFeatureName(translated->feature) == QStringLiteral("translation"),
            "features have stable names");

    for (const QString& kind :
         {QStringLiteral("table_extract"), QStringLiteral("table_vision_extract")}) {
        const auto sample =
            usageSampleForDiagnosticEvent(QtInfoMsg, category, QStringLiteral("request.finished"),
                                          {{QStringLiteral("request_kind"), kind}}, 0);
        require(sample.has_value() && sample->feature == UsageFeature::TableExtraction,
                "table extraction requests carry their feature");
    }
    for (const QString& kind :
         {QStringLiteral("latex_extract"), QStringLiteral("latex_vision_extract")}) {
        const auto sample =
            usageSampleForDiagnosticEvent(QtInfoMsg, category, QStringLiteral("request.finished"),
                                          {{QStringLiteral("request_kind"), kind}}, 0);
        require(sample.has_value() && sample->feature == UsageFeature::LatexExtraction,
                "latex extraction requests carry their feature");
    }
    require(!usageSampleForDiagnosticEvent(
                 QtInfoMsg, category, QStringLiteral("request.finished"),
                 {{QStringLiteral("request_kind"), QStringLiteral("chat_models")}}, 0)
                 .has_value(),
            "catalogue refreshes are not user activity");
}

void annotationEventsCarryToolAndSurface() {
    const QString category = QStringLiteral("snow_shot.annotation");
    const auto selected = usageSampleForDiagnosticEvent(
        QtInfoMsg, category, QStringLiteral("annotation.tool_selected"),
        {{QStringLiteral("tool"), QStringLiteral("arrow")},
         {QStringLiteral("surface"), QStringLiteral("capture")}},
        11);
    require(selected.has_value() && selected->event == UsageEvent::AnnotationToolSelected,
            "tool selection maps to its own event");
    require(selected->tool == UsageTool::Arrow && selected->surface == UsageSurface::Capture,
            "tool selection keeps its tool and surface");
    require(selected->epochMs == 11, "the caller owns the annotation timestamp");
    require(usageToolName(selected->tool) == QStringLiteral("arrow") &&
                usageSurfaceName(selected->surface) == QStringLiteral("capture"),
            "annotation dimensions have stable names");

    const auto committed = usageSampleForDiagnosticEvent(
        QtInfoMsg, category, QStringLiteral("annotation.stroke_committed"),
        {{QStringLiteral("tool"), QStringLiteral("pen_filter")},
         {QStringLiteral("surface"), QStringLiteral("image_editing")}},
        12);
    require(committed.has_value() && committed->event == UsageEvent::AnnotationStrokeCommitted &&
                committed->tool == UsageTool::PenFilter &&
                committed->surface == UsageSurface::ImageEditing,
            "stroke commits keep their tool and surface");

    require(!usageSampleForDiagnosticEvent(QtInfoMsg, category,
                                           QStringLiteral("annotation.stroke_committed"),
                                           {{QStringLiteral("tool"),
                                             QStringLiteral("does_not_exist")}},
                                           0)
                 .has_value(),
            "an unknown tool is dropped instead of being counted anonymously");
    require(!usageSampleForDiagnosticEvent(QtInfoMsg, category,
                                           QStringLiteral("annotation.tool_selected"), {}, 0)
                 .has_value(),
            "an annotation event without a tool is not counted");

    const auto noSurface = usageSampleForDiagnosticEvent(
        QtInfoMsg, category, QStringLiteral("annotation.tool_selected"),
        {{QStringLiteral("tool"), QStringLiteral("text")}}, 0);
    require(noSurface.has_value() && noSurface->surface == UsageSurface::Unknown,
            "a missing surface still produces a countable sample");
    require(usageSurfaceName(noSurface->surface).isEmpty(),
            "an unknown surface is omitted rather than guessed");

    QSet<QString> toolNames;
    for (quint8 index = 1; index < static_cast<quint8>(UsageTool::Count); ++index) {
        const QString name = usageToolName(static_cast<UsageTool>(index));
        require(!name.isEmpty(), "every catalogued tool has a name");
        require(!toolNames.contains(name), "tool names are unique");
        toolNames.insert(name);
    }
    require(toolNames.size() == 21, "the catalogue covers every annotation tool");
}

void annotationSamplesReachTheBatchWithTheirDimensions() {
    FakeSeqServer server;
    require(server.listen(), "the stand-in collector listens");
    require(start(fastOptions(server.endpoint())), "usage telemetry starts");
    recordUsage(QStringLiteral("annotation.tool_selected"),
                {{QStringLiteral("tool"), QStringLiteral("arrow")},
                 {QStringLiteral("surface"), QStringLiteral("capture")}});
    recordUsage(QStringLiteral("annotation.stroke_committed"),
                {{QStringLiteral("tool"), QStringLiteral("eraser")}});
    require(waitFor([&] { return !server.requests.isEmpty(); }), "a batch reaches the collector");

    bool sawSelected = false;
    bool sawCommitted = false;
    for (const QByteArray& line : linesOf(bodyOf(server.requests.first()))) {
        const QJsonObject record = QJsonDocument::fromJson(line).object();
        const QString eventName = record.value(QStringLiteral("EventName")).toString();
        if (eventName == QStringLiteral("annotation.tool_selected")) {
            sawSelected = true;
            require(record.value(QStringLiteral("Tool")).toString() == QStringLiteral("arrow"),
                    "the batch carries the selected tool");
            require(record.value(QStringLiteral("Surface")).toString() ==
                        QStringLiteral("capture"),
                    "the batch carries the surface");
        } else if (eventName == QStringLiteral("annotation.stroke_committed")) {
            sawCommitted = true;
            require(record.value(QStringLiteral("Tool")).toString() == QStringLiteral("eraser"),
                    "the batch carries the committed tool");
            require(!record.contains(QStringLiteral("Surface")),
                    "an unknown surface is omitted from the record");
        }
    }
    require(sawSelected && sawCommitted, "both annotation events reach the collector");
    stop();
}

void batchesSamplesIntoOneClefRequest() {
    FakeSeqServer server;
    require(server.listen(), "the stand-in collector listens");
    require(start(fastOptions(server.endpoint())), "usage telemetry starts");
    recordUsage(QStringLiteral("capture.finished"));
    recordUsage(QStringLiteral("ocr.finished"),
                {{QStringLiteral("outcome"), QStringLiteral("succeeded")}});
    recordUsage(QStringLiteral("request.finished"),
                {{QStringLiteral("request_kind"), QStringLiteral("table_extract")},
                 {QStringLiteral("outcome"), QStringLiteral("succeeded")}});
    require(waitFor([&] { return !server.requests.isEmpty(); }), "a batch reaches the collector");

    const QByteArray request = server.requests.first();
    require(request.startsWith("POST "), "the batch is posted");
    require(request.contains("application/vnd.serilog.clef"),
            "the batch declares the CLEF content type");
    const QList<QByteArray> lines = linesOf(bodyOf(request));
    require(lines.size() == 4, "one session marker plus the three recorded events");

    QSet<QString> names;
    for (const QByteArray& line : lines) {
        const QJsonDocument document = QJsonDocument::fromJson(line);
        require(document.isObject(), "every line is a self-contained JSON object");
        const QJsonObject record = document.object();
        require(record.value(QStringLiteral("Count")).toInt() == 1, "each line counts once");
        require(record.value(QStringLiteral("UserId")).toString() == QStringLiteral("unit-test"),
                "each line carries the user id");
        require(record.value(QStringLiteral("AppName")).toString() == QStringLiteral("SnowShot"),
                "each line carries the application name");
        require(!record.value(QStringLiteral("Date")).toString().isEmpty(),
                "each line carries a local date");
        require(!record.value(QStringLiteral("@t")).toString().isEmpty(),
                "each line is timestamped");
        require(record.value(QStringLiteral("@mt")).toString().contains(QStringLiteral("usage")),
                "each line carries a readable template");
        require(record.value(QStringLiteral("SessionId")).toString() == QStringLiteral("session-1"),
                "each line carries the session");
        names.insert(record.value(QStringLiteral("EventName")).toString());
        if (record.value(QStringLiteral("EventName")).toString() ==
            QStringLiteral("api.request_finished")) {
            require(record.value(QStringLiteral("Feature")).toString() ==
                        QStringLiteral("table_extraction"),
                    "the API request reports its feature");
        }
        if (record.value(QStringLiteral("EventName")).toString() ==
            QStringLiteral("capture.finished"))
            require(!record.contains(QStringLiteral("Outcome")),
                    "unknown outcomes are omitted rather than guessed");
    }
    require(names == QSet<QString>(
                         {QStringLiteral("session.start"), QStringLiteral("capture.finished"),
                          QStringLiteral("ocr.finished"), QStringLiteral("api.request_finished")}),
            "the batch carries exactly the expected event names");

    const auto began = std::chrono::steady_clock::now();
    stop();
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);
    require(spent < std::chrono::milliseconds(2000), "shutdown stays inside its budget");
    const qsizetype afterStop = server.requests.size();
    spin(200);
    require(server.requests.size() == afterStop, "nothing is sent after shutdown");
}

void rejectedBatchesBackOff() {
    FakeSeqServer server;
    server.status = 500;
    require(server.listen(), "the rejecting collector listens");
    require(start(fastOptions(server.endpoint())), "usage telemetry starts");
    recordUsage(QStringLiteral("capture.finished"));
    require(waitFor([&] { return !server.requests.isEmpty(); }), "the first batch is attempted");
    const qsizetype attempts = server.requests.size();
    spin(400);
    require(server.requests.size() == attempts,
            "a failure backs off instead of retrying every flush window");
    stop();
}

void unreachableCollectorIsSilent() {
    quint16 deadPort = 0;
    {
        QTcpServer probe;
        require(probe.listen(QHostAddress::LocalHost), "a free port is available");
        deadPort = probe.serverPort();
    }
    Options options = fastOptions(
        QUrl(QStringLiteral("http://127.0.0.1:%1/seq/api/events/raw?clef").arg(deadPort)));
    require(start(options), "usage telemetry starts against a dead endpoint");
    recordUsage(QStringLiteral("capture.finished"));
    spin(300);
    const auto began = std::chrono::steady_clock::now();
    stop();
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);
    require(spent < std::chrono::milliseconds(2000), "a dead endpoint cannot stall shutdown");
}

void disabledTelemetryCollectsNothing() {
    Options options = fastOptions(QUrl(QStringLiteral("http://127.0.0.1:9/seq")));
    options.enabled = false;
    require(!start(options), "a disabled collector refuses to start");
    const quint64 before = droppedSampleCount();
    for (int index = 0; index < 600; ++index)
        recordUsage(QStringLiteral("capture.finished"));
    require(droppedSampleCount() == before,
            "a disabled collector never installs the tap, so nothing is buffered");
    stop();
}

void ringDropsOverflowDeterministically() {
    const quint64 before = droppedSampleCount();
    installDiagnosticTap();
    for (int index = 0; index < 600; ++index)
        recordUsage(QStringLiteral("capture.finished"));
    require(droppedSampleCount() - before == 88,
            "the ring keeps 512 samples and counts the overflow instead of growing");

    FakeSeqServer server;
    require(server.listen(), "the stand-in collector listens");
    Options options = fastOptions(server.endpoint());
    options.flushInterval = std::chrono::milliseconds(5000);
    require(start(options), "usage telemetry starts on a full ring");
    require(waitFor([&] { return !server.requests.isEmpty(); }), "the full ring still flushes");
    const QList<QByteArray> lines = linesOf(bodyOf(server.requests.first()));
    require(lines.size() <= 100, "a single request never exceeds the batch ceiling");
    require(!lines.isEmpty(), "the batch is not empty");
    stop();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        catalogueMapsDiagnosticsEvents();
        annotationEventsCarryToolAndSurface();
        annotationSamplesReachTheBatchWithTheirDimensions();
        batchesSamplesIntoOneClefRequest();
        rejectedBatchesBackOff();
        unreachableCollectorIsSilent();
        disabledTelemetryCollectsNothing();
        ringDropsOverflowDeterministically();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

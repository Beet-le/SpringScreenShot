#ifndef SNOW_SHOT_TELEMETRY_USAGETELEMETRY_H
#define SNOW_SHOT_TELEMETRY_USAGETELEMETRY_H

#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <chrono>

namespace snow_shot::telemetry {

// Records which features people actually use. Samples are collected from the diagnostics event
// stream through a tap, buffered in a fixed-size ring, and posted to the internal Seq instance
// in batches. Nothing here is allowed to become load-bearing for the application:
//
// * the collector never blocks the recording thread beyond one short mutex and a 24-byte copy,
// * the sender is asynchronous and single-flight, so a hung or unreachable endpoint cannot
//   stall the UI, and
// * memory, request size, and request rate are all fixed constants, independent of how much
//   the user works.
//
// Failures are dropped rather than persisted. A batch has no de-duplication key, so replaying
// it after an ambiguous failure would double-count; undercounting only flattens usage trends,
// while double-counting would systematically inflate them. The 60 s flush window bounds the
// worst-case loss, including a hard shutdown that skips aboutToQuit.

// Installs the diagnostics tap that feeds the ring. Safe before QCoreApplication exists and
// safe to call from any thread; calling it twice is a no-op.
void installDiagnosticTap() noexcept;
void uninstallDiagnosticTap() noexcept;

// Samples dropped because the ring was full since the process started.
quint64 droppedSampleCount() noexcept;

struct Options {
    // The internal Seq collector. Hard-coded on purpose: it is a company intranet endpoint,
    // provisioned per deployment, and this is the single sanctioned exception to the
    // "never hard-code endpoints" rule. Tests override it with a loopback URL.
    QUrl endpoint = QUrl(QStringLiteral("https://desktop.cqczy.cn/seq/api/events/raw?clef"));
    QString userId;  // defaults to the profile directory name, e.g. "024723"
    QString appName; // "SnowShot" or "SnowShotMini"
    QString appVersion;
    QString sessionId;
    bool useSystemProxy = false;
    bool enabled = true; // tests use false to prove no traffic leaves the process
    std::chrono::milliseconds firstFlushDelay = std::chrono::seconds(20);
    std::chrono::milliseconds flushInterval = std::chrono::seconds(60);
    std::chrono::milliseconds transferTimeout = std::chrono::seconds(10);
};

// Creates the sender and arms the flush window. Main thread only, idempotent, and completely
// silent on failure: telemetry must never be a reason the application misbehaves.
bool start(Options options);

// Uninstalls the tap, makes one bounded attempt to deliver what is still queued, and releases
// the network manager. Idempotent, main thread only, and never blocks for longer than the
// shutdown budget.
void stop() noexcept;

void setUseSystemProxy(bool enabled) noexcept;

} // namespace snow_shot::telemetry

#endif

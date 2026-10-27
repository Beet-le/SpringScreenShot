#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

using namespace snow_shot::diagnostics;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

DiagnosticsOptions optionsFor(const QString& directory) {
    DiagnosticsOptions options;
    options.directories = {directory};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    options.mirrorToConsole = false;
    return options;
}

struct Capture {
    int calls = 0;
    QtMsgType level = QtInfoMsg;
    QString category;
    QString event;
    QJsonObject fields;
};

void capturingSink(void* context, QtMsgType level, const QString& category, const QString& event,
                   const QJsonObject& fields) noexcept {
    auto* capture = static_cast<Capture*>(context);
    ++capture->calls;
    capture->level = level;
    capture->category = category;
    capture->event = event;
    capture->fields = fields;
}

// A sink is a foreign body running on the recording thread; recording from inside it is a
// mistake that must degrade to "no extra sample" rather than infinite recursion.
void recordingSink(void* context, QtMsgType level, const QString& category, const QString& event,
                   const QJsonObject& fields) noexcept {
    capturingSink(context, level, category, event, fields);
    logEvent(QStringLiteral("snow_shot.test"), QStringLiteral("sink.nested"));
}

void tapDeliversRecordedEvents() {
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-tap-XXXXXX"));
    DiagnosticsService service;
    require(service.initialize(optionsFor(directory.path())), "logger initializes");
    Capture capture;
    setEventTap({&capturingSink, &capture});
    service.record(QtWarningMsg, QStringLiteral("snow_shot.test"),
                   QStringLiteral("capture.finished"), {},
                   {{QStringLiteral("outcome"), QStringLiteral("succeeded")}});
    require(capture.calls == 1, "the tap receives the recorded event");
    require(capture.level == QtWarningMsg, "the tap receives the level");
    require(capture.category == QStringLiteral("snow_shot.test"), "the tap receives the category");
    require(capture.event == QStringLiteral("capture.finished"), "the tap receives the event name");
    require(capture.fields.value(QStringLiteral("outcome")).toString() ==
                QStringLiteral("succeeded"),
            "the tap receives the fields");

    setEventTap({});
    service.record(QtWarningMsg, QStringLiteral("snow_shot.test"),
                   QStringLiteral("capture.finished"));
    require(capture.calls == 1, "a cleared tap stops delivery");
    service.shutdown();
}

// Filtering is the sink's job: the tap must not pre-filter anything, or a sink could never
// explain why a log record it expected is missing from the usage stream.
void tapDeliversQtMessages() {
    Capture capture;
    setEventTap({&capturingSink, &capture});
    logEvent(QStringLiteral("snow_shot.app"), QStringLiteral("qt.message"), {});
    require(capture.calls == 1 && capture.event == QStringLiteral("qt.message"),
            "qt.message reaches the tap");
    setEventTap({});
}

void sinkMayRecordWithoutRecursing() {
    Capture capture;
    setEventTap({&recordingSink, &capture});
    logEvent(QStringLiteral("snow_shot.test"), QStringLiteral("capture.finished"));
    require(capture.calls == 1, "a sink that records does not re-enter the tap");
    setEventTap({});
}

// The load-bearing property for usage counting: a missing or unwritable log directory must not
// take the observer down with it, so the tap runs before the "is the logger running" gate.
void tapRunsWithoutAnInitializedLogger() {
    DiagnosticsService service;
    Capture capture;
    setEventTap({&capturingSink, &capture});
    service.record(QtInfoMsg, QStringLiteral("snow_shot.test"), QStringLiteral("capture.finished"));
    require(capture.calls == 1, "the tap survives a logger that never started");
    setEventTap({});
    service.shutdown();
}

void tapDoesNotDisturbRecordedOutput() {
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-tap-XXXXXX"));
    DiagnosticsService service;
    require(service.initialize(optionsFor(directory.path())), "logger initializes");
    Capture capture;
    setEventTap({&capturingSink, &capture});
    service.record(QtInfoMsg, QStringLiteral("snow_shot.test"), QStringLiteral("queue.marker"),
                   QStringLiteral("recorded body"));
    require(service.flush(), "the logger flushes with a tap installed");
    require(capture.calls == 1, "the tap counted the record as well");
    QFile file(service.status().currentFile);
    require(file.open(QIODevice::ReadOnly), "the log file is readable");
    const QByteArray bytes = file.readAll();
    require(bytes.contains("queue.marker") && bytes.contains("recorded body"),
            "the tap never suppresses or rewrites diagnostics output");
    setEventTap({});
    service.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        tapDeliversRecordedEvents();
        tapDeliversQtMessages();
        sinkMayRecordWithoutRecursing();
        tapRunsWithoutAnInitializedLogger();
        tapDoesNotDisturbRecordedOutput();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

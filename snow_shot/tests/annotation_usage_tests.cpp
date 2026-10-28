#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/presentation/screenshotcanvastoolmapping.h"
#include "snow_shot/telemetry/annotationusage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace snow_shot::telemetry;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct CapturedEvent {
    QString category;
    QString event;
    QJsonObject fields;
};

std::vector<CapturedEvent> captured;

void capturingSink(void*, QtMsgType, const QString& category, const QString& event,
                   const QJsonObject& fields) noexcept {
    try {
        captured.push_back({category, event, fields});
    } catch (...) {
        // The tap contract is noexcept; a test that cannot record is still a test that must
        // not take the process down.
    }
}

// Installs the capture for one scenario, so each case sees only its own reports.
class TapScope {
  public:
    TapScope() : m_previous(snow_shot::diagnostics::EventTap{}) {
        captured.clear();
        snow_shot::diagnostics::EventTap tap;
        tap.sink = &capturingSink;
        tap.context = nullptr;
        snow_shot::diagnostics::setEventTap(tap);
    }
    ~TapScope() {
        snow_shot::diagnostics::setEventTap(m_previous);
    }

  private:
    snow_shot::diagnostics::EventTap m_previous;
};

void everyAnnotationToolHasAStableName() {
    QStringList names;
    for (const auto& descriptor : snow_shot::presentation::screenshotCanvasActiveTools) {
        const QString name = annotationToolName(descriptor.canvasTool);
        require(!name.isEmpty(), "every annotation tool has a name");
        require(!names.contains(name), "annotation tool names are unique");
        require(name == name.toLower(), "annotation tool names are snake_case");
        names.append(name);
        require(annotationToolName(descriptor.activeTool) == name,
                "the toolbar enum and the canvas enum agree on the name");
    }
    require(names.size() == 21, "the shared command list still covers every annotation tool");
}

void recognitionToolsAreNotAnnotations() {
    for (const auto tool :
         {ScreenshotActiveTool::Move, ScreenshotActiveTool::Ocr, ScreenshotActiveTool::Table,
          ScreenshotActiveTool::Qr, ScreenshotActiveTool::Markdown, ScreenshotActiveTool::Html,
          ScreenshotActiveTool::Latex, ScreenshotActiveTool::TextTranslation}) {
        require(annotationToolName(tool).isEmpty(), "recognition tools are not annotation tools");
    }
}

void repeatedSelectionIsReportedOnce() {
    TapScope scope;
    // Two identical selections are one user decision; the third call switches surface and must
    // therefore count again.
    reportAnnotationToolSelected(SnowCanvasTool::Arrow, UsageSurface::Pinned);
    reportAnnotationToolSelected(SnowCanvasTool::Arrow, UsageSurface::Pinned);
    reportAnnotationToolSelected(SnowCanvasTool::Arrow, UsageSurface::Capture);
    reportAnnotationToolSelected(SnowCanvasTool::Line, UsageSurface::Pinned);

    require(captured.size() == 3, "identical selections collapse into a single report");
    require(captured.front().category == QStringLiteral("snow_shot.annotation"),
            "annotation reports use their own diagnostics category");
    require(captured.front().event == QStringLiteral("annotation.tool_selected"),
            "tool reports use the tool event");
    require(captured.front().fields.value(QStringLiteral("tool")).toString() ==
                QStringLiteral("arrow"),
            "tool reports carry the tool");
    require(captured.front().fields.value(QStringLiteral("surface")).toString() ==
                QStringLiteral("pinned"),
            "tool reports carry the surface");
    require(captured[1].fields.value(QStringLiteral("surface")).toString() ==
                QStringLiteral("capture"),
            "the same tool on another surface still counts");
    require(captured.back().fields.value(QStringLiteral("tool")).toString() ==
                QStringLiteral("line"),
            "switching tools still counts");
}

void everyStrokeIsReported() {
    TapScope scope;
    // Committing strokes is never deduplicated: three strokes are three samples.
    reportAnnotationStrokeCommitted(SnowCanvasTool::Eraser, UsageSurface::ImageEditing);
    reportAnnotationStrokeCommitted(SnowCanvasTool::Eraser, UsageSurface::ImageEditing);
    reportAnnotationStrokeCommitted(SnowCanvasTool::Eraser, UsageSurface::ImageEditing);

    require(captured.size() == 3, "every committed stroke is reported");
    for (const CapturedEvent& record : captured) {
        require(record.category == QStringLiteral("snow_shot.annotation"),
                "stroke reports use their own diagnostics category");
        require(record.event == QStringLiteral("annotation.stroke_committed"),
                "stroke reports use the stroke event");
        require(record.fields.value(QStringLiteral("tool")).toString() == QStringLiteral("eraser"),
                "stroke reports carry the tool");
        require(record.fields.value(QStringLiteral("surface")).toString() ==
                    QStringLiteral("image_editing"),
                "stroke reports carry the surface");
    }
}

// The production reporter and a real diagnostics service have to agree with each other: the
// dimensions must survive persistence, not only the in-memory event tap.
void reportedDimensionsSurvivePersistence() {
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-annotation-diag-XXXXXX"));
    require(directory.isValid(), "annotation diagnostics sandbox is available");
    snow_shot::diagnostics::DiagnosticsOptions options;
    options.directories = {directory.path()};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    options.mirrorToConsole = false;
    // The production reporters go through DiagnosticsService::instance(), so the assertion has
    // to run against the singleton to cover the real path.
    auto& service = snow_shot::diagnostics::DiagnosticsService::instance();
    require(service.initialize(options), "annotation diagnostics initialize");

    reportAnnotationStrokeCommitted(SnowCanvasTool::Arrow, UsageSurface::Capture);
    reportAnnotationStrokeCommitted(SnowCanvasTool::Arrow, UsageSurface::Pinned);

    const auto exported = service.exportDay(QDate::currentDate()).get();
    require(exported.success, "annotation records export");
    int captures = 0;
    int pinned = 0;
    QFile file(exported.path);
    require(file.open(QIODevice::ReadOnly), "annotation records are readable");
    for (const QByteArray& line : file.readAll().split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        if (record.value(QStringLiteral("event")) != QStringLiteral("annotation.stroke_committed"))
            continue;
        const auto fields = record.value(QStringLiteral("fields")).toObject();
        require(fields.value(QStringLiteral("tool")).toString() == QStringLiteral("arrow"),
                "the persisted record keeps the tool dimension");
        const QString surface = fields.value(QStringLiteral("surface")).toString();
        if (surface == QStringLiteral("capture"))
            ++captures;
        else if (surface == QStringLiteral("pinned"))
            ++pinned;
        else
            require(false, "the persisted record keeps a known surface dimension");
    }
    // Committed strokes are never deduplicated, so both surfaces have to survive as well.
    require(captures == 1 && pinned == 1,
            "every committed stroke reaches the persisted record with its surface");
    service.shutdown();
}

void unknownSurfacesAreOmittedFromTheRecord() {
    TapScope scope;
    reportAnnotationStrokeCommitted(SnowCanvasTool::Text, UsageSurface::Unknown);
    require(captured.size() == 1, "a stroke without a surface is still reported");
    require(!captured.front().fields.contains(QStringLiteral("surface")),
            "an unknown surface is omitted rather than guessed");
    require(captured.front().fields.value(QStringLiteral("tool")).toString() ==
                QStringLiteral("text"),
            "the tool survives a missing surface");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        everyAnnotationToolHasAStableName();
        recognitionToolsAreNotAnnotations();
        repeatedSelectionIsReportedOnce();
        everyStrokeIsReported();
        unknownSurfacesAreOmittedFromTheRecord();
        reportedDimensionsSurvivePersistence();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}

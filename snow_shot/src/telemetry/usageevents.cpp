#include "snow_shot/telemetry/usageevents.h"

#include <QLatin1String>
#include <QList>

#include <array>

namespace snow_shot::telemetry {
namespace {
struct EventRule {
    const char* diagnosticEvent;
    UsageEvent event;
};

struct ToolRule {
    const char* name;
    UsageTool tool;
};

struct SurfaceRule {
    const char* name;
    UsageSurface surface;
};

// One row per diagnostics event the catalogue counts. Any feature that already records a
// diagnostics event therefore becomes measurable by adding a single line here, with no
// change to the recording sites and no extra work in the uploader.
constexpr std::array<EventRule, 26> kRules{{
    {"session.unexpected_termination", UsageEvent::SessionUnexpectedTermination},
    {"capture.finished", UsageEvent::CaptureFinished},
    {"scrolling.started", UsageEvent::ScrollingStarted},
    {"annotation.tool_selected", UsageEvent::AnnotationToolSelected},
    {"annotation.stroke_committed", UsageEvent::AnnotationStrokeCommitted},
    {"ocr.submitted", UsageEvent::OcrSubmitted},
    {"ocr.finished", UsageEvent::OcrFinished},
    {"ocr.cancelled", UsageEvent::OcrCancelled},
    {"recording.started", UsageEvent::RecordingStarted},
    {"recording.paused", UsageEvent::RecordingPaused},
    {"recording.resumed", UsageEvent::RecordingResumed},
    {"recording.export_finished", UsageEvent::RecordingExportFinished},
    {"recording.failed", UsageEvent::RecordingFailed},
    {"recording.render_failed", UsageEvent::RecordingFailed},
    {"recording.trim_ready", UsageEvent::RecordingTrimOpened},
    {"export.image_finished", UsageEvent::ExportImageFinished},
    {"export.cancelled", UsageEvent::ExportCancelled},
    {"pin.started", UsageEvent::PinStarted},
    {"pin.finished", UsageEvent::PinFinished},
    {"pin.rejected", UsageEvent::PinRejected},
    {"clipboard.finished", UsageEvent::ClipboardFinished},
    {"print.snapshot_requested", UsageEvent::PrintSnapshotRequested},
    {"print.capture_failed", UsageEvent::PrintFailed},
    {"print.snapshot_failed", UsageEvent::PrintFailed},
    {"print.pinned_failed", UsageEvent::PrintFailed},
    {"request.finished", UsageEvent::ApiRequestFinished},
}};

// Only consulted once an annotation event matched, so the linear scan never runs for the
// diagnostics traffic that dominates the tap.
constexpr std::array<ToolRule, 21> kToolRules{{
    {"select", UsageTool::Select},
    {"shape", UsageTool::Shape},
    {"arrow", UsageTool::Arrow},
    {"line", UsageTool::Line},
    {"free_draw", UsageTool::FreeDraw},
    {"rectangle_highlight", UsageTool::RectangleHighlight},
    {"pen_highlight", UsageTool::PenHighlight},
    {"eraser", UsageTool::Eraser},
    {"rectangle_filter", UsageTool::RectangleFilter},
    {"watermark", UsageTool::Watermark},
    {"text", UsageTool::Text},
    {"serial_number", UsageTool::SerialNumber},
    {"pen_filter", UsageTool::PenFilter},
    {"spotlight", UsageTool::Spotlight},
    {"auto_filter", UsageTool::AutoFilter},
    {"rectangle_eraser", UsageTool::RectangleEraser},
    {"brush_eraser", UsageTool::BrushEraser},
    {"distance", UsageTool::Distance},
    {"angle", UsageTool::Angle},
    {"magnifier", UsageTool::Magnifier},
    {"speech_bubble", UsageTool::SpeechBubble},
}};

constexpr std::array<SurfaceRule, 3> kSurfaceRules{{
    {"capture", UsageSurface::Capture},
    {"pinned", UsageSurface::Pinned},
    {"image_editing", UsageSurface::ImageEditing},
}};

constexpr QLatin1StringView kDiagnosticsCategoryPrefix("snow_shot");
// Emitted for every Qt log record by the diagnostics message handler; by far the most common
// value to arrive here, so it is rejected before the category even gets inspected.
constexpr QLatin1StringView kQtMessageEvent("qt.message");

UsageOutcome outcomeFromField(const QJsonObject& fields) {
    const QString value = fields.value(QStringLiteral("outcome")).toString();
    if (value == QLatin1String("succeeded"))
        return UsageOutcome::Succeeded;
    if (value == QLatin1String("failed"))
        return UsageOutcome::Failed;
    if (value == QLatin1String("cancelled"))
        return UsageOutcome::Cancelled;
    if (value == QLatin1String("rejected"))
        return UsageOutcome::Rejected;
    return UsageOutcome::Unknown;
}

// Only reached for API requests. "chat_models" is the model catalogue refresh, which says
// nothing about how the user works and would otherwise dominate the request counts.
std::optional<UsageFeature> featureFromRequestKind(const QString& value) {
    if (value == QLatin1String("translation"))
        return UsageFeature::Translation;
    if (value == QLatin1String("table_extract") || value == QLatin1String("table_vision_extract"))
        return UsageFeature::TableExtraction;
    if (value == QLatin1String("latex_extract") || value == QLatin1String("latex_vision_extract"))
        return UsageFeature::LatexExtraction;
    if (value == QLatin1String("image_conversion"))
        return UsageFeature::ImageConversion;
    if (value.isEmpty())
        return UsageFeature::None;
    return std::nullopt;
}

// A sample without a tool would be indistinguishable from every other tool, so an unknown
// annotation tool is dropped instead of being uploaded anonymously.
std::optional<UsageTool> toolFromField(const QJsonObject& fields) {
    const QString value = fields.value(QStringLiteral("tool")).toString();
    if (value.isEmpty())
        return std::nullopt;
    for (const ToolRule& rule : kToolRules) {
        if (value == QLatin1String(rule.name))
            return rule.tool;
    }
    return std::nullopt;
}

// The surface is a nice-to-have dimension: an unrecognised value still produces a usable row.
UsageSurface surfaceFromField(const QJsonObject& fields) {
    const QString value = fields.value(QStringLiteral("surface")).toString();
    for (const SurfaceRule& rule : kSurfaceRules) {
        if (value == QLatin1String(rule.name))
            return rule.surface;
    }
    return UsageSurface::Unknown;
}
} // namespace

std::optional<UsageSample> usageSampleForDiagnosticEvent(QtMsgType level, const QString& category,
                                                         const QString& event,
                                                         const QJsonObject& fields,
                                                         qint64 epochMs) noexcept {
    if (level == QtDebugMsg)
        return std::nullopt;
    if (event == kQtMessageEvent)
        return std::nullopt;
    if (!category.startsWith(kDiagnosticsCategoryPrefix))
        return std::nullopt;

    const EventRule* match = nullptr;
    for (const EventRule& rule : kRules) {
        if (event == QLatin1String(rule.diagnosticEvent)) {
            match = &rule;
            break;
        }
    }
    if (match == nullptr)
        return std::nullopt;

    UsageSample sample;
    sample.epochMs = epochMs;
    sample.event = match->event;
    sample.outcome = outcomeFromField(fields);
    if (sample.event == UsageEvent::ApiRequestFinished) {
        const auto feature =
            featureFromRequestKind(fields.value(QStringLiteral("request_kind")).toString());
        if (!feature)
            return std::nullopt;
        sample.feature = *feature;
    }
    if (sample.event == UsageEvent::AnnotationToolSelected ||
        sample.event == UsageEvent::AnnotationStrokeCommitted) {
        const auto tool = toolFromField(fields);
        if (!tool)
            return std::nullopt;
        sample.tool = *tool;
        sample.surface = surfaceFromField(fields);
    }
    return sample;
}

QString usageEventName(UsageEvent event) {
    switch (event) {
    case UsageEvent::SessionStart:
        return QStringLiteral("session.start");
    case UsageEvent::SessionCleanShutdown:
        return QStringLiteral("session.clean_shutdown");
    case UsageEvent::SessionUnexpectedTermination:
        return QStringLiteral("session.unexpected_termination");
    case UsageEvent::CaptureFinished:
        return QStringLiteral("capture.finished");
    case UsageEvent::ScrollingStarted:
        return QStringLiteral("scrolling.started");
    case UsageEvent::OcrSubmitted:
        return QStringLiteral("ocr.submitted");
    case UsageEvent::OcrFinished:
        return QStringLiteral("ocr.finished");
    case UsageEvent::OcrCancelled:
        return QStringLiteral("ocr.cancelled");
    case UsageEvent::RecordingStarted:
        return QStringLiteral("recording.started");
    case UsageEvent::RecordingPaused:
        return QStringLiteral("recording.paused");
    case UsageEvent::RecordingResumed:
        return QStringLiteral("recording.resumed");
    case UsageEvent::RecordingExportFinished:
        return QStringLiteral("recording.export_finished");
    case UsageEvent::RecordingFailed:
        return QStringLiteral("recording.failed");
    case UsageEvent::RecordingTrimOpened:
        return QStringLiteral("recording.trim_opened");
    case UsageEvent::ExportImageFinished:
        return QStringLiteral("export.image_finished");
    case UsageEvent::ExportCancelled:
        return QStringLiteral("export.cancelled");
    case UsageEvent::PinStarted:
        return QStringLiteral("pin.started");
    case UsageEvent::PinFinished:
        return QStringLiteral("pin.finished");
    case UsageEvent::PinRejected:
        return QStringLiteral("pin.rejected");
    case UsageEvent::ClipboardFinished:
        return QStringLiteral("clipboard.finished");
    case UsageEvent::PrintSnapshotRequested:
        return QStringLiteral("print.snapshot_requested");
    case UsageEvent::PrintFailed:
        return QStringLiteral("print.failed");
    case UsageEvent::ApiRequestFinished:
        return QStringLiteral("api.request_finished");
    case UsageEvent::AnnotationToolSelected:
        return QStringLiteral("annotation.tool_selected");
    case UsageEvent::AnnotationStrokeCommitted:
        return QStringLiteral("annotation.stroke_committed");
    case UsageEvent::Count:
        break;
    }
    return {};
}

QString usageOutcomeName(UsageOutcome outcome) {
    switch (outcome) {
    case UsageOutcome::Succeeded:
        return QStringLiteral("succeeded");
    case UsageOutcome::Failed:
        return QStringLiteral("failed");
    case UsageOutcome::Cancelled:
        return QStringLiteral("cancelled");
    case UsageOutcome::Rejected:
        return QStringLiteral("rejected");
    case UsageOutcome::Unknown:
        break;
    }
    return {};
}

QString usageFeatureName(UsageFeature feature) {
    switch (feature) {
    case UsageFeature::Translation:
        return QStringLiteral("translation");
    case UsageFeature::TableExtraction:
        return QStringLiteral("table_extraction");
    case UsageFeature::LatexExtraction:
        return QStringLiteral("latex_extraction");
    case UsageFeature::ImageConversion:
        return QStringLiteral("image_conversion");
    case UsageFeature::Other:
        return QStringLiteral("other");
    case UsageFeature::None:
        break;
    }
    return {};
}

QString usageToolName(UsageTool tool) {
    switch (tool) {
    case UsageTool::Select:
        return QStringLiteral("select");
    case UsageTool::Shape:
        return QStringLiteral("shape");
    case UsageTool::Arrow:
        return QStringLiteral("arrow");
    case UsageTool::Line:
        return QStringLiteral("line");
    case UsageTool::FreeDraw:
        return QStringLiteral("free_draw");
    case UsageTool::RectangleHighlight:
        return QStringLiteral("rectangle_highlight");
    case UsageTool::PenHighlight:
        return QStringLiteral("pen_highlight");
    case UsageTool::Eraser:
        return QStringLiteral("eraser");
    case UsageTool::RectangleFilter:
        return QStringLiteral("rectangle_filter");
    case UsageTool::Watermark:
        return QStringLiteral("watermark");
    case UsageTool::Text:
        return QStringLiteral("text");
    case UsageTool::SerialNumber:
        return QStringLiteral("serial_number");
    case UsageTool::PenFilter:
        return QStringLiteral("pen_filter");
    case UsageTool::Spotlight:
        return QStringLiteral("spotlight");
    case UsageTool::AutoFilter:
        return QStringLiteral("auto_filter");
    case UsageTool::RectangleEraser:
        return QStringLiteral("rectangle_eraser");
    case UsageTool::BrushEraser:
        return QStringLiteral("brush_eraser");
    case UsageTool::Distance:
        return QStringLiteral("distance");
    case UsageTool::Angle:
        return QStringLiteral("angle");
    case UsageTool::Magnifier:
        return QStringLiteral("magnifier");
    case UsageTool::SpeechBubble:
        return QStringLiteral("speech_bubble");
    case UsageTool::Unknown:
    case UsageTool::Count:
        break;
    }
    return {};
}

QString usageSurfaceName(UsageSurface surface) {
    switch (surface) {
    case UsageSurface::Capture:
        return QStringLiteral("capture");
    case UsageSurface::Pinned:
        return QStringLiteral("pinned");
    case UsageSurface::ImageEditing:
        return QStringLiteral("image_editing");
    case UsageSurface::Unknown:
    case UsageSurface::Count:
        break;
    }
    return {};
}

QStringList trackedDiagnosticEvents() {
    QStringList names;
    names.reserve(static_cast<qsizetype>(kRules.size()));
    for (const EventRule& rule : kRules)
        names.append(QString::fromLatin1(rule.diagnosticEvent));
    return names;
}

} // namespace snow_shot::telemetry

#include "snow_shot/telemetry/annotationusage.h"

#include "snow_shot/diagnostics/diagnostics.h"
#include "snow_shot/presentation/screenshotcanvastoolmapping.h"

#include <QJsonObject>

#include <array>
#include <atomic>
#include <cstddef>

namespace snow_shot::telemetry {
namespace {

// Re-selecting the tool that is already active is the common case for a toolbar that never
// clears its selection, so those repeats are dropped before they can push real activity out
// of the 512-slot ring buffer.
constexpr std::size_t kSurfaceSlots = static_cast<std::size_t>(UsageSurface::Count);
std::array<std::atomic<unsigned>, kSurfaceSlots> selectedTool{};

// Engine enum onto the catalogue dimension. The switch stays exhaustive without a default so
// a tool added upstream fails to compile here instead of silently going unreported.
UsageTool toolForCanvasTool(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Select:
        return UsageTool::Select;
    case SnowCanvasTool::Shape:
        return UsageTool::Shape;
    case SnowCanvasTool::Arrow:
        return UsageTool::Arrow;
    case SnowCanvasTool::Line:
        return UsageTool::Line;
    case SnowCanvasTool::FreeDraw:
        return UsageTool::FreeDraw;
    case SnowCanvasTool::RectangleHighlight:
        return UsageTool::RectangleHighlight;
    case SnowCanvasTool::PenHighlight:
        return UsageTool::PenHighlight;
    case SnowCanvasTool::Eraser:
        return UsageTool::Eraser;
    case SnowCanvasTool::RectangleFilter:
        return UsageTool::RectangleFilter;
    case SnowCanvasTool::Watermark:
        return UsageTool::Watermark;
    case SnowCanvasTool::Text:
        return UsageTool::Text;
    case SnowCanvasTool::SerialNumber:
        return UsageTool::SerialNumber;
    case SnowCanvasTool::PenFilter:
        return UsageTool::PenFilter;
    case SnowCanvasTool::Spotlight:
        return UsageTool::Spotlight;
    case SnowCanvasTool::AutoFilter:
        return UsageTool::AutoFilter;
    case SnowCanvasTool::RectangleEraser:
        return UsageTool::RectangleEraser;
    case SnowCanvasTool::BrushEraser:
        return UsageTool::BrushEraser;
    case SnowCanvasTool::Distance:
        return UsageTool::Distance;
    case SnowCanvasTool::Angle:
        return UsageTool::Angle;
    case SnowCanvasTool::Magnifier:
        return UsageTool::Magnifier;
    case SnowCanvasTool::SpeechBubble:
        return UsageTool::SpeechBubble;
    }
    return UsageTool::Unknown;
}

QString annotationEventName(bool stroke) {
    return stroke ? QStringLiteral("annotation.stroke_committed")
                  : QStringLiteral("annotation.tool_selected");
}

void reportAnnotationEvent(SnowCanvasTool tool, UsageSurface surface, bool stroke) noexcept {
    try {
        const QString name = annotationToolName(tool);
        if (name.isEmpty())
            return;
        QJsonObject fields;
        fields.insert(QStringLiteral("tool"), name);
        const QString surfaceName = usageSurfaceName(surface);
        if (!surfaceName.isEmpty())
            fields.insert(QStringLiteral("surface"), surfaceName);
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.annotation"),
                                         annotationEventName(stroke), fields);
    } catch (...) {
        // Telemetry must never propagate into the drawing path.
    }
}
} // namespace

QString annotationToolName(SnowCanvasTool tool) {
    return usageToolName(toolForCanvasTool(tool));
}

QString annotationToolName(ScreenshotActiveTool tool) {
    // Driven by the shared command list so a tool added to the toolbar cannot be counted
    // under a stale name here.
    for (const auto& descriptor : presentation::screenshotCanvasActiveTools) {
        if (descriptor.activeTool == tool)
            return annotationToolName(descriptor.canvasTool);
    }
    return {};
}

void reportAnnotationToolSelected(SnowCanvasTool tool, UsageSurface surface) noexcept {
    const auto slot = static_cast<std::size_t>(surface);
    if (slot < selectedTool.size()) {
        const unsigned current = static_cast<unsigned>(tool);
        if (selectedTool[slot].exchange(current) == current)
            return;
    }
    reportAnnotationEvent(tool, surface, false);
}

void reportAnnotationStrokeCommitted(SnowCanvasTool tool, UsageSurface surface) noexcept {
    reportAnnotationEvent(tool, surface, true);
}

} // namespace snow_shot::telemetry

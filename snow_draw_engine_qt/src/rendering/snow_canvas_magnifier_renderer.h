#pragma once

#include "snow_draw_engine.h"
#include "snow_draw_engine_qt/snow_canvas_image_source_types.h"

#include <QList>
#include <QPainterPath>

#include <cstddef>
#include <cstdint>

class QPainter;
class SnowCanvasSceneItem;
struct SceneDisplayInfo;

namespace snow_canvas_magnifier_renderer {

struct RenderDiagnostics {
    std::size_t renderedMagnifierCount = 0;
    std::size_t sourceLayerDrawCount = 0;
    std::size_t compositedTileCount = 0;
    std::size_t compositedPixelCount = 0;
    std::size_t peakCompositingPixelCount = 0;
    std::size_t geometryBuildCount = 0;
    std::size_t geometryReuseCount = 0;
    std::size_t bridgeHullBuildCount = 0;
    std::size_t bridgeHullReuseCount = 0;
    std::size_t disjointBridgeDrawCount = 0;
    std::size_t maskedBridgeDrawCount = 0;
    std::uint64_t connectionClipNanoseconds = 0;
    std::uint64_t connectionFillNanoseconds = 0;
    std::uint64_t sourceOutlineNanoseconds = 0;
};

RenderDiagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();
void setStageTimingsEnabledForCurrentThread(bool enabled);
// Benchmark-only controls; normal rendering retains the antialiased gradient.
void setBridgeBenchmarkOptionsForCurrentThread(bool solidFill, bool disableAntialiasing);
QPainterPath lensPath(const SnowSceneDisplayItem& item);
QPainterPath sourcePath(const SnowSceneDisplayItem& item);
void render(QPainter& painter, const SceneDisplayInfo& displayInfo, const SnowCanvasSceneItem& item,
            const QList<SnowCanvasBaseImageSource>* sources);

} // namespace snow_canvas_magnifier_renderer

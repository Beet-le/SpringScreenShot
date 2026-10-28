#ifndef SNOW_SHOT_TELEMETRY_ANNOTATIONUSAGE_H
#define SNOW_SHOT_TELEMETRY_ANNOTATIONUSAGE_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/telemetry/usageevents.h"

#include <QString>

namespace snow_shot::telemetry {

// The snake_case names are the values Seq groups by, so they must stay independent of the
// toolbar wording. An empty result means "this is not an annotation tool".
QString annotationToolName(SnowCanvasTool tool);
QString annotationToolName(ScreenshotActiveTool tool);

// Both report at most one sample per user action and swallow every failure: the canvas
// handlers that call them are on the drawing path and must never notice telemetry at all.
void reportAnnotationToolSelected(SnowCanvasTool tool, UsageSurface surface) noexcept;
void reportAnnotationStrokeCommitted(SnowCanvasTool tool, UsageSurface surface) noexcept;

} // namespace snow_shot::telemetry

#endif

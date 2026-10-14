#pragma once

#include "snow_draw_engine_qt/snow_canvas_wheel_input.h"

#include <QWheelEvent>

namespace snow_shot::presentation {

inline bool usesPreciseWheelDelta(const QWheelEvent& event) {
    return snow_canvas_wheel::usesPreciseDelta(event);
}

// The canvas library owns discrete adjustment thresholds and gesture boundaries.
using WheelStepAccumulator = snow_canvas_wheel::StepAccumulator;

} // namespace snow_shot::presentation

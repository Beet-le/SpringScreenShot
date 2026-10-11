#pragma once

#include "snow_draw_engine.h"

#include <cstdint>

class QWidget;
class SnowCanvasCursorController;

namespace snow_canvas_interaction {

class Controller final {
  public:
    bool isEnabled() const;

    void setEnabled(QWidget& widget, SnowCanvasCursorController& cursorController, bool enabled);
    void clearTransientState(QWidget& widget, SnowCanvasCursorController& cursorController);
    void syncPointerCapture(QWidget& widget, bool active);
    void applyOutput(QWidget& widget, SnowCanvasCursorController& cursorController,
                     const SnowInteractionOutput& output);

  private:
    void releasePointerCapture(QWidget& widget);

    bool m_enabled = true;
    std::uint32_t m_capturedPointerId = 0;
};

} // namespace snow_canvas_interaction

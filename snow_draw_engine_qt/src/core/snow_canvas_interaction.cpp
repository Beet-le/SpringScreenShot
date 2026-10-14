#include "snow_canvas_interaction.h"

#include "snow_canvas_cursor_controller.h"
#include "snow_canvas_input_adapter.h"

#include <QWidget>
#include <QPointer>

namespace snow_canvas_interaction {

bool Controller::isEnabled() const {
    return m_enabled;
}

void Controller::setEnabled(QWidget& widget, SnowCanvasCursorController& cursorController,
                            bool enabled) {
    if (m_enabled == enabled) {
        return;
    }

    m_enabled = enabled;
    if (!m_enabled) {
        clearTransientState(widget, cursorController);
    }
}

void Controller::clearTransientState(QWidget& widget,
                                     SnowCanvasCursorController& cursorController) {
    const QPointer<QWidget> lifetime(&widget);
    releasePointerCapture(widget);
    if (!lifetime) {
        return;
    }
    cursorController.clearCursor(SnowCanvasCursorLayer::CanvasTool);
}

void Controller::releasePointerCapture(QWidget& widget) {
    if (m_capturedPointerId != 0) {
        m_capturedPointerId = 0;
        widget.releaseMouse();
    }
}

void Controller::syncPointerCapture(QWidget& widget, bool active) {
    if (!active) {
        releasePointerCapture(widget);
    }
}

void Controller::applyOutput(QWidget& widget, SnowCanvasCursorController& cursorController,
                             const SnowInteractionOutput& output) {
    if (!m_enabled) {
        return;
    }
    const QPointer<QWidget> lifetime(&widget);

    switch (output.capture_kind) {
    case SNOW_POINTER_CAPTURE_CAPTURE:
        if (m_capturedPointerId == 0) {
            m_capturedPointerId = output.capture_pointer_id;
            widget.grabMouse();
        } else {
            m_capturedPointerId = output.capture_pointer_id;
        }
        break;
    case SNOW_POINTER_CAPTURE_RELEASE:
        releasePointerCapture(widget);
        break;
    case SNOW_POINTER_CAPTURE_NO_CHANGE:
    default:
        break;
    }
    if (!lifetime) {
        return;
    }

    if (output.cursor_kind == SNOW_CURSOR_SET) {
        cursorController.setEngineCursor(output.cursor_style);
    }
}

} // namespace snow_canvas_interaction

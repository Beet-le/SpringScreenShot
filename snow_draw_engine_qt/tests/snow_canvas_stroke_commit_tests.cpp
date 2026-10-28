#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point), button, buttons, Qt::NoModifier);
    event.setAccepted(false);
    QApplication::sendEvent(&canvas, &event);
    return event.isAccepted();
}

bool tablet(SnowCanvasWidget& canvas, const QPointingDevice& device, QEvent::Type type,
            QPointF point) {
    const auto button = type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton;
    QTabletEvent event(type, &device, point, canvas.mapToGlobal(point), 1.0, 0, 0, 0, 0, 0,
                       Qt::NoModifier, button, buttons);
    event.setAccepted(false);
    QApplication::sendEvent(&canvas, &event);
    return event.isAccepted();
}

void drag(SnowCanvasWidget& canvas, QPointF from, QPointF to) {
    mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, (from + to) / 2.0, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

std::unique_ptr<SnowCanvasWidget> makeCanvas(SnowCanvasRuntime& runtime) {
    auto canvas = std::make_unique<SnowCanvasWidget>(runtime);
    canvas->resize(400, 300);
    canvas->show();
    QApplication::processEvents();
    return canvas;
}

void dragReportsOneStrokeWithItsTool() {
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::Arrow), "activate arrow");
    std::vector<SnowCanvasTool> reported;
    canvas->setStrokeCommittedHandler(
        [&reported](SnowCanvasTool tool) { reported.push_back(tool); });
    drag(*canvas, {60, 60}, {260, 160});
    require(reported.size() == 1, "one drag reports exactly one stroke");
    require(reported.front() == SnowCanvasTool::Arrow, "the stroke reports the arrow tool");
    require(runtime.hasDocumentContent(), "the stroke reached the document");

    drag(*canvas, {80, 200}, {280, 240});
    require(reported.size() == 2, "every further drag is reported once");
}

void movesWithoutAPressReportNothing() {
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::Arrow), "activate arrow");
    int reported = 0;
    canvas->setStrokeCommittedHandler([&reported](SnowCanvasTool) { ++reported; });
    mouse(*canvas, QEvent::MouseMove, {60, 60}, Qt::LeftButton, Qt::LeftButton);
    mouse(*canvas, QEvent::MouseMove, {160, 160}, Qt::NoButton, Qt::LeftButton);
    require(reported == 0, "a hover never counts as a stroke");
    require(!runtime.hasDocumentContent(), "a hover never changes the document");
}

void selectingReportsNothing() {
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::Arrow), "activate arrow");
    int reported = 0;
    canvas->setStrokeCommittedHandler([&reported](SnowCanvasTool) { ++reported; });
    drag(*canvas, {60, 60}, {260, 160});
    require(reported == 1, "the fixture stroke is reported");

    require(canvas->setCanvasTool(SnowCanvasTool::Select), "switch to select");
    drag(*canvas, {100, 90}, {180, 150});
    require(reported == 1, "moving an element with Select is not a stroke");
}

void canvasesWithoutAHandlerKeepWorking() {
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::Arrow), "activate arrow");
    drag(*canvas, {60, 60}, {260, 160});
    require(runtime.hasDocumentContent() && canvas->canvasHistoryState().canUndo,
            "a canvas without a handler still draws normally");
    drag(*canvas, {80, 200}, {280, 240});
    require(canvas->undo() && canvas->undo() && !runtime.hasDocumentContent(),
            "and still records one history entry per stroke");
}

void stylusStrokesAreReportedOnce() {
    QPointingDevice pen(QStringLiteral("stroke tablet"), 61, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::FreeDraw), "activate free draw");
    std::vector<SnowCanvasTool> reported;
    canvas->setStrokeCommittedHandler(
        [&reported](SnowCanvasTool tool) { reported.push_back(tool); });
    tablet(*canvas, pen, QEvent::TabletPress, {60, 60});
    tablet(*canvas, pen, QEvent::TabletMove, {140, 90});
    tablet(*canvas, pen, QEvent::TabletMove, {220, 120});
    tablet(*canvas, pen, QEvent::TabletRelease, {220, 120});
    require(reported.size() == 1, "one stylus stroke reports exactly one sample");
    require(reported.front() == SnowCanvasTool::FreeDraw, "the stylus stroke reports free draw");
    require(runtime.hasDocumentContent(), "the stylus stroke reached the document");
}

void stylusSelectionReportsNothing() {
    QPointingDevice pen(QStringLiteral("selection tablet"), 62, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position, 1,
                        3);
    SnowCanvasRuntime runtime;
    auto canvas = makeCanvas(runtime);
    require(canvas->setCanvasTool(SnowCanvasTool::Arrow), "activate arrow");
    int reported = 0;
    canvas->setStrokeCommittedHandler([&reported](SnowCanvasTool) { ++reported; });
    drag(*canvas, {60, 60}, {260, 160});
    require(reported == 1, "the fixture stroke is reported");

    require(canvas->setCanvasTool(SnowCanvasTool::Select), "switch to select");
    tablet(*canvas, pen, QEvent::TabletPress, {100, 90});
    tablet(*canvas, pen, QEvent::TabletMove, {140, 120});
    tablet(*canvas, pen, QEvent::TabletRelease, {140, 120});
    require(reported == 1, "a stylus select gesture is not a stroke");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    dragReportsOneStrokeWithItsTool();
    movesWithoutAPressReportNothing();
    selectingReportsNothing();
    canvasesWithoutAHandlerKeepWorking();
    stylusStrokesAreReportedOnce();
    stylusSelectionReportsNothing();
    std::cout << "Stroke commit callback tests passed\n";
    return EXIT_SUCCESS;
}

#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"

#include <QPainterPath>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct WheelFixture {
    selection_presentation_test::Fixture presentation{QSize(1200, 800), false};
    std::unique_ptr<ScreenshotSelectionEditWorkflow> workflow;
    std::unique_ptr<ScreenshotOverlayInputHandler> input;
    int updates = 0;
    int toolbarMoves = 0;

    WheelFixture() {
        presentation.interaction.setMoveTool(true, false);
        presentation.captureState.sessionState = ScreenshotSessionState::Editing;
        ScreenshotSelectionEditUiActions ui;
        ui.updateOverlayState = [this] {
            ++updates;
            presentation.services->updateOverlayState();
        };
        ui.showSelectionToolbar = [this] { presentation.services->showSelectionToolbar(); };
        ui.moveToolbar = [this] {
            ++toolbarMoves;
            presentation.services->moveToolbar();
        };
        workflow = std::make_unique<ScreenshotSelectionEditWorkflow>(
            ScreenshotSelectionEditWorkflowContext{presentation.overlay, presentation.captureState,
                                                   presentation.displays, presentation.geometry,
                                                   presentation.interaction, presentation.selection,
                                                   std::move(ui)});
        ScreenshotOverlayInputActions actions;
        actions.adjustSelectionFromToolbar = [this](int minDx, int minDy, int maxDx, int maxDy) {
            workflow->adjustSelectionFromToolbar(minDx, minDy, maxDx, maxDy);
        };
        input =
            std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
                presentation.captureState, presentation.interaction, presentation.selection,
                presentation.intelligentSelection, presentation.geometry, presentation.displays,
                std::move(actions)});
        select(QRectF(200, 200, 300, 180));
    }

    void select(const QRectF& selection) {
        presentation.selection.setSelectionRect(selection);
        presentation.services->updateOverlayState();
        presentation.services->showToolbar();
        presentation.services->showSelectionToolbar();
        presentation.flushFrame();
        presentation.processEvents();
        updates = 0;
        toolbarMoves = 0;
        input->resetTransientShortcuts();
    }

    bool scroll(QPoint angles, QPoint pixels = {}, Qt::ScrollPhase phase = Qt::NoScrollPhase,
                QPointF pointer = QPointF(210, 210)) {
        QWheelEvent event(pointer, presentation.overlay.mapToGlobal(pointer.toPoint()), pixels,
                          angles, Qt::NoButton, Qt::NoModifier, phase, false);
        return input->handleWheel(&presentation.overlay, event);
    }

    QRectF selection() const {
        return presentation.selection.normalizedSelection();
    }
};

void wheelMovesEveryEdgeByOneUnitAndSynchronizesToolbars() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    auto* mainToolbar = fixture.presentation.coordinator.toolbar();
    auto* selectionToolbar = fixture.presentation.coordinator.selectionToolbar();
    require(mainToolbar && selectionToolbar && mainToolbar->isVisible() &&
                selectionToolbar->isVisible(),
            "wheel fixture must show both screenshot toolbars");
    const QPoint mainPosition = mainToolbar->pos();
    const QPoint selectionPosition = selectionToolbar->pos();
    require(fixture.scroll(QPoint(0, 120), QPoint(0, 30)) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1) &&
                fixture.selection().center() == original.center(),
            "one mouse notch must move every edge by one canvas unit around the center");
    require(fixture.updates == 1 && fixture.toolbarMoves == 1 &&
                mainToolbar->pos() != mainPosition && selectionToolbar->pos() != selectionPosition,
            "both screenshot toolbars must move synchronously with wheel geometry");
    require(fixture.scroll(QPoint(0, -120), {}, Qt::NoScrollPhase, QPointF(900, 700)) &&
                fixture.selection() == original && mainToolbar->pos() == mainPosition &&
                selectionToolbar->pos() == selectionPosition,
            "reverse scrolling must restore geometry and toolbar positions regardless of pointer");
    require(fixture.scroll(QPoint(0, 360)) &&
                fixture.selection() == original.adjusted(-3, -3, 3, 3) && fixture.updates == 3,
            "coalesced notches must apply all integer edge offsets in one update");

    fixture.select(original);
    require(fixture.presentation.selection.setAspectRatioLockEnabled(true, 1),
            "enable an existing aspect ratio lock");
    require(fixture.scroll(QPoint(0, 120)) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1),
            "centered wheel edits must use direct offsets without proportional scaling");
}

void centeredWheelStopsAtCanvasAndMinimumSize() {
    WheelFixture fixture;
    const QRectF bounds = fixture.presentation.geometry.canvasBounds();
    const QRectF nearEdge(bounds.left() + 1, bounds.top() + 20, 100, 80);
    fixture.select(nearEdge);
    require(fixture.scroll(QPoint(0, 1200)) &&
                fixture.selection() == nearEdge.adjusted(-1, -1, 1, 1) &&
                fixture.selection().center() == nearEdge.center(),
            "expansion must stop at the nearest canvas edge without moving the center");
    require(fixture.scroll(QPoint(0, 120)) && fixture.updates == 1 && fixture.toolbarMoves == 1,
            "scrolling against a canvas limit must not issue redundant presentation updates");
    const QRectF nearBottomRight(bounds.right() - 101, bounds.bottom() - 81, 100, 80);
    fixture.select(nearBottomRight);
    require(fixture.scroll(QPoint(0, 1200)) &&
                fixture.selection() == nearBottomRight.adjusted(-1, -1, 1, 1),
            "right and bottom canvas limits must constrain the same centered edge offsets");

    const QRectF small(200, 200, 3, 5);
    fixture.select(small);
    require(fixture.scroll(QPoint(0, -1200)) &&
                fixture.selection() == small.adjusted(1, 1, -1, -1) &&
                fixture.selection().center() == small.center(),
            "coalesced shrinking must stop at one unit without flipping the rectangle");
    require(fixture.scroll(QPoint(0, -120)) && fixture.updates == 1 && fixture.toolbarMoves == 1,
            "scrolling against the minimum size must leave the toolbars and geometry unchanged");
    require(fixture.scroll(QPoint(0, 120)) && fixture.selection() == small,
            "reversing direction at a size limit must respond immediately");
}

void preciseWheelAccumulatesAndResetsWithInputOwnership() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    require(fixture.scroll({}, {}, Qt::ScrollBegin) &&
                fixture.scroll({}, QPoint(20, 0), Qt::ScrollUpdate) && fixture.updates == 0,
            "empty and horizontal wheel input must not resize the selection");
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) &&
                fixture.selection() == original.adjusted(-1, -1, 1, 1),
            "precise scrolling must respond with one centered unit immediately");
    require(fixture.scroll({}, QPoint(0, 99), Qt::ScrollUpdate) && fixture.updates == 1,
            "small precise movements must accumulate instead of resizing on every event");
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) &&
                fixture.selection() == original.adjusted(-2, -2, 2, 2),
            "the next precise threshold must apply exactly one more unit");
    require(fixture.scroll({}, QPoint(0, 500), Qt::ScrollMomentum) &&
                fixture.scroll({}, QPoint(0, 500), Qt::ScrollEnd) && fixture.updates == 2,
            "momentum and scroll-end deltas must not resize the selection");

    fixture.input->setExternalDragActive(true);
    require(fixture.scroll(QPoint(0, 120)) && fixture.updates == 2,
            "external drag must consume wheel input without changing selection geometry");
    fixture.input->setExternalDragActive(false);
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) && fixture.updates == 3,
            "resizing after external drag must start with a fresh precise wheel accumulator");
    fixture.presentation.interaction.setCanvasTool(ScreenshotActiveTool::Text);
    require(!fixture.scroll({}, QPoint(0, 99), Qt::ScrollUpdate) && fixture.updates == 3,
            "drawing tools must retain wheel ownership without resizing the screenshot");
    fixture.presentation.interaction.setMoveTool(true, false);
    require(fixture.scroll({}, QPoint(0, 1), Qt::ScrollUpdate) && fixture.updates == 4,
            "reactivating Edit Selection must discard the previous tool's wheel remainder");
}

void wheelDoesNotReplaceCustomRegionsOrInterruptSelectionGestures() {
    WheelFixture fixture;
    const QRectF original = fixture.selection();
    require(fixture.presentation.interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All),
            "start an Edit Selection drag");
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "wheel edits must not interrupt a selection drag");
    fixture.presentation.interaction.finishDrag();
    fixture.presentation.interaction.setMoveTool(true, false);
    require(fixture.presentation.interaction.beginEffectDrag(
                {ScreenshotSelectionEffectHandle::Shadow, {}, 0, 0, 0, 1, {}}),
            "start a selection effect drag");
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "wheel edits must not interrupt a selection effect drag");
    fixture.presentation.interaction.finishEffectDrag();
    fixture.presentation.interaction.enterScrollingCapture();
    require(!fixture.scroll(QPoint(0, 120)) && fixture.selection() == original,
            "scrolling capture must retain the wheel without resizing the selection");
    fixture.presentation.interaction.setMoveTool(true, false);
    QPainterPath path;
    path.addEllipse(original);
    fixture.presentation.selection.setSelectionRegion(
        ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve));
    const auto custom = fixture.presentation.selection.selectionRegion();
    require(!fixture.scroll(QPoint(0, 120)) &&
                fixture.presentation.selection.selectionRegion() == custom && fixture.updates == 0,
            "wheel edits must not replace a custom region with a rectangle");
    fixture.presentation.selection.clearSelection();
    require(!fixture.scroll(QPoint(0, 120)) && !fixture.presentation.selection.hasPixelSelection(),
            "wheel input must not create a selection when none exists");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    selection_presentation_test::IsolatedStorage storage;
    wheelMovesEveryEdgeByOneUnitAndSynchronizesToolbars();
    centeredWheelStopsAtCanvasAndMinimumSize();
    preciseWheelAccumulatesAndResetsWithInputOwnership();
    wheelDoesNotReplaceCustomRegionsOrInterruptSelectionGestures();
    return 0;
}

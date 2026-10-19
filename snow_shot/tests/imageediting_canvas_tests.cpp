// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPointingDevice>
#include <QTabletEvent>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, point, point, canvas.mapToGlobal(point), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void drag(SnowCanvasWidget& canvas, QPointF start, QPointF end,
          QEvent::Type press = QEvent::MouseButtonPress) {
    mouse(canvas, press, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto elementSlots = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                  .object()
                                  .value(QStringLiteral("document"))
                                  .toObject()
                                  .value(QStringLiteral("slots"))
                                  .toArray();
    QJsonArray result;
    for (const auto& slot : elementSlots) {
        if (slot.toObject().value(QStringLiteral("data")).toObject().contains(kind))
            result.append(slot);
    }
    return result;
}

QJsonObject rectangle(const SnowCanvasRuntime& runtime) {
    const auto shapes = records(runtime, QStringLiteral("Rectangle"));
    require(shapes.size() == 1, "canvas fixture retains exactly one rectangle");
    return shapes.first()
        .toObject()
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("Rectangle"))
        .toObject();
}

struct Fixture {
    ImageEditingDocument document;
    ImageEditingCanvas canvas{document};

    Fixture() {
        canvas.resize(480, 400);
        canvas.show();
        QApplication::processEvents();
        ImageEditingSource source;
        source.image = QImage(240, 160, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(QColor(40, 50, 60));
        source.canvasRect = QRectF(0, 0, 240, 160);
        require(document.load(source), "load canvas fixture image");
        canvas.setMode(ImageEditingCanvas::Mode::Draw);
        require(canvas.setViewportCamera(120, 80, 1), "use a deterministic image camera");
        require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate canvas shape tool");
    }

    QPointF view(QPointF canvasPoint) const {
        return canvas.canvasToViewTransform().map(canvasPoint);
    }

    void addRectangle(const QRectF& bounds) {
        const QJsonObject operation{
            {QStringLiteral("type"), QStringLiteral("rectangle")},
            {QStringLiteral("bounds"),
             QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}},
            {QStringLiteral("style"),
             QJsonObject{{QStringLiteral("fill"), QJsonArray{255, 0, 0, 255}},
                         {QStringLiteral("stroke"), QJsonArray{0, 0, 0, 0}}}}};
        require(!document.runtime()
                     .applyAnnotationTransaction(
                         QJsonDocument(
                             QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("operations"), QJsonArray{operation}}})
                             .toJson(QJsonDocument::Compact))
                     .isEmpty(),
                "add canvas fixture rectangle");
        require(canvas.resetEditingState(), "clear fixture selection");
        require(canvas.setCanvasTool(SnowCanvasTool::Shape), "restore shape tool");
    }

    void selectRectangle() {
        const auto id = records(document.runtime(), QStringLiteral("Rectangle"))
                            .first()
                            .toObject()
                            .value(QStringLiteral("id"))
                            .toObject();
        SnowCanvasRuntimeEditor editor(document.runtime());
        require(editor.select(static_cast<quint32>(id.value(QStringLiteral("index")).toInt()),
                              static_cast<quint32>(id.value(QStringLiteral("generation")).toInt())),
                "select canvas fixture rectangle");
        QApplication::processEvents();
    }

    QImage exported() {
        return document.runtime().renderToImage(document.bounds(), document.image().size(),
                                                {{document.image(), document.bounds()}});
    }
};

void resizeControlsRemainVisibleAndUsableAtEveryImageEdge() {
    struct Edge {
        QRectF shape;
        QPointF handle;
        QPointF dragOffset;
        QRectF externalArea;
    };
    // Selection frames sit four view pixels outside the shape. Each sampled
    // area lies wholly beyond the image, where clipping used to hide the handle.
    const std::array edges{
        Edge{{1, 50, 50, 40}, {-3, 46}, {-16, -10}, {-7, 41, 7, 11}},
        Edge{{189, 50, 50, 40}, {243, 46}, {16, -10}, {241, 41, 7, 11}},
        Edge{{90, 1, 50, 40}, {86, -3}, {-10, -16}, {81, -7, 11, 7}},
        Edge{{90, 119, 50, 40}, {144, 163}, {10, 16}, {139, 161, 11, 7}},
    };
    for (const auto& edge : edges) {
        Fixture fixture;
        fixture.addRectangle(edge.shape);
        const auto area =
            fixture.canvas.canvasToViewTransform().mapRect(edge.externalArea).toAlignedRect();
        const auto withoutControl = fixture.canvas.grab(area).toImage();
        const auto exported = fixture.exported();
        fixture.selectRectangle();
        require(fixture.canvas.grab(area).toImage() != withoutControl,
                "selection resize controls remain visible beyond every image edge");
        require(fixture.exported() == exported, "editor controls do not enter exported pixels");
        const auto before = rectangle(fixture.document.runtime());
        drag(fixture.canvas, fixture.view(edge.handle),
             fixture.view(edge.handle + edge.dragOffset));
        require(rectangle(fixture.document.runtime()) != before &&
                    records(fixture.document.runtime(), QStringLiteral("Rectangle")).size() == 1,
                "an outside-image resize control modifies the existing shape");
    }
}

void rotationAndTextControlsRemainUsableOutsideImage() {
    Fixture rotation;
    rotation.addRectangle({90, 1, 50, 40});
    const auto rotationPoint = rotation.view({115, -23});
    const QRect rotationArea(rotationPoint.toPoint() - QPoint(6, 6), QSize(13, 13));
    const auto withoutRotation = rotation.canvas.grab(rotationArea).toImage();
    rotation.selectRectangle();
    require(rotation.canvas.grab(rotationArea).toImage() != withoutRotation,
            "the rotation control remains visible above the image");
    drag(rotation.canvas, rotationPoint, rotationPoint + QPointF(25, 10));
    require(
        std::abs(
            rectangle(rotation.document.runtime()).value(QStringLiteral("rotation")).toDouble()) >
            0.1,
        "the outside-image rotation control rotates the existing shape");

    for (const auto press : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        Fixture text;
        text.addRectangle({90, 119, 50, 40});
        const auto textPoint = text.view({115, 183});
        const QRect textArea(textPoint.toPoint() - QPoint(6, 6), QSize(13, 13));
        const auto withoutText = text.canvas.grab(textArea).toImage();
        text.selectRectangle();
        require(text.canvas.grab(textArea).toImage() != withoutText,
                "the attached-text control remains visible below the image");
        drag(text.canvas, textPoint, textPoint, press);
        require(text.canvas.hasActiveTextEditing(),
                "single and double clicks on external controls open attached text editing");
        QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"));
        QApplication::sendEvent(&text.canvas, &input);
        mouse(text.canvas, QEvent::MouseButtonPress, textPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(text.canvas, QEvent::MouseButtonRelease, textPoint, Qt::LeftButton, Qt::NoButton);
        require(text.canvas.hasActiveTextEditing(),
                "an active inline editor remains interactive outside the image");
        QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
        QApplication::sendEvent(&text.canvas, &commit);
        require(!text.canvas.hasActiveTextEditing() &&
                    records(text.document.runtime(), QStringLiteral("Text")).size() == 1,
                "attached text commits as a single existing-shape editing operation");
    }
}

void documentPixelsRemainClippedWhileControlsAreVisible() {
    Fixture fixture;
    const QRect external =
        fixture.canvas.canvasToViewTransform().mapRect(QRectF(-12, 56, 8, 28)).toAlignedRect();
    const auto emptyPixels = fixture.canvas.grab(external).toImage();
    fixture.addRectangle({-15, 50, 30, 40});
    require(fixture.canvas.grab(external).toImage() == emptyPixels,
            "document pixels extending past the image remain clipped in the editor");
    const auto exported = fixture.exported();
    require(exported.size() == fixture.document.image().size() &&
                exported.pixelColor(0, 70) == QColor(Qt::red),
            "export keeps image dimensions and includes the visible part of the annotation");
    fixture.selectRectangle();
    require(fixture.exported() == exported,
            "export excludes selection overlays beyond image edges");
}

void blankOutsideGesturesCannotCreateAnnotations() {
    for (const auto tool :
         {SnowCanvasTool::Shape, SnowCanvasTool::Arrow, SnowCanvasTool::Text,
          SnowCanvasTool::FreeDraw, SnowCanvasTool::PenFilter, SnowCanvasTool::BrushEraser}) {
        for (const auto press : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
            Fixture fixture;
            require(fixture.canvas.setCanvasTool(tool), "activate outside-gesture fixture tool");
            const auto before = fixture.document.checkpoint();
            drag(fixture.canvas, fixture.view({-30, 80}), fixture.view({100, 80}), press);
            require(fixture.document.checkpoint() == before &&
                        !fixture.canvas.hasActiveTextEditing(),
                    "blank outside-image mouse gestures cannot create annotations or drafts");
        }
    }
    Fixture fixture;
    require(fixture.canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate native tablet tool");
    const auto before = fixture.document.checkpoint();
    const QPointingDevice pen(
        QStringLiteral("image editor test pen"), 54, QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const auto tablet = [&](QEvent::Type type, QPointF position, quint64 timestamp) {
        QTabletEvent event(type, &pen, position, fixture.canvas.mapToGlobal(position), 0.5, 0, 0, 0,
                           0, 0, Qt::NoModifier,
                           type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                           type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        event.setTimestamp(timestamp);
        QApplication::sendEvent(&fixture.canvas, &event);
    };
    tablet(QEvent::TabletPress, fixture.view({-30, 80}), 100);
    tablet(QEvent::TabletMove, fixture.view({100, 80}), 110);
    tablet(QEvent::TabletRelease, fixture.view({100, 80}), 120);
    require(fixture.document.checkpoint() == before,
            "blank outside-image native tablet gestures cannot create annotations");

    fixture.canvas.setMode(ImageEditingCanvas::Mode::CropRows);
    int crops = 0;
    fixture.canvas.cropRequested = [&](ImageEditingDocument::CropAxis, int, int) { ++crops; };
    drag(fixture.canvas, fixture.view({-30, 80}), fixture.view({100, 120}));
    require(crops == 0, "crop gestures still require an image starting point");
    drag(fixture.canvas, fixture.view({100, 80}), fixture.view({100, 120}));
    require(crops == 1, "inside-image crop gestures remain functional");
}
void cropPreviewsRepaintOnlyTheirDamageAndCancelCleanly() {
    for (const auto mode :
         {ImageEditingCanvas::Mode::CropRows, ImageEditingCanvas::Mode::CropColumns}) {
        Fixture fixture;
        fixture.canvas.setMode(mode);
        QApplication::processEvents();
        const auto before = fixture.canvas.grab().toImage();
        class PaintRegions final : public QObject {
          public:
            QRegion painted;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Paint)
                    painted += static_cast<QPaintEvent*>(event)->region();
                return false;
            }
        } events;
        fixture.canvas.installEventFilter(&events);
        mouse(fixture.canvas, QEvent::MouseButtonPress, fixture.view({20, 20}), Qt::LeftButton,
              Qt::LeftButton);
        events.painted = {};
        mouse(fixture.canvas, QEvent::MouseMove, fixture.view({26, 26}), Qt::NoButton,
              Qt::LeftButton);
        const auto damage = events.painted.boundingRect();
        require(!damage.isEmpty() && (mode == ImageEditingCanvas::Mode::CropRows
                                          ? damage.height() < fixture.canvas.height() / 2
                                          : damage.width() < fixture.canvas.width() / 2),
                "crop motion repaints the preview strip without repainting the full canvas");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&fixture.canvas, &escape);
        QApplication::processEvents();
        require(fixture.canvas.grab().toImage() == before && !fixture.document.dirty(),
                "cancelling the preview restores every pixel without editing the document");
    }
}
} // namespace

void runImageEditingCanvasTests() {
    resizeControlsRemainVisibleAndUsableAtEveryImageEdge();
    rotationAndTextControlsRemainUsableOutsideImage();
    documentPixelsRemainClippedWhileControlsAreVisible();
    blankOutsideGesturesCannotCreateAnnotations();
    cropPreviewsRepaintOnlyTheirDamageAndCancelCleanly();
}

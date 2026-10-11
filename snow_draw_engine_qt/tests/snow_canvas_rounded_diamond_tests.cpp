#include "snow_canvas_display_item.h"
#include "snow_canvas_magnifier_renderer.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_spotlight_renderer.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTransform>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

SceneDisplayInfo projection(double zoom = 1.0) {
    SceneDisplayInfo info{};
    info.surface_width = 200;
    info.surface_height = 200;
    info.camera_zoom = zoom;
    return info;
}

QImage renderShape(const SnowSceneDisplayItem& item, double dpr = 1.0, double zoom = 1.0) {
    QImage image(QSize(qRound(200 * dpr), qRound(200 * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QImage source(200, 200, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::blue);
    const QList<SnowCanvasBaseImageSource> sources{{source, QRectF(-100, -100, 200, 200), {}}};
    const SnowCanvasSceneItem sceneItem(item);
    const auto info = projection(zoom);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = &sceneItem;
    request.sceneItemCount = 1;
    request.exposedRegion = QRegion(QRect(0, 0, 200, 200));
    request.clearBackgroundEnabled = false;
    request.baseImageSources = &sources;
    snow_canvas_renderer::renderSceneItems(request);
    return image;
}

QColor pixel(const QImage& image, double x, double y) {
    return image.pixelColor(qFloor(x * image.devicePixelRatio()),
                            qFloor(y * image.devicePixelRatio()));
}

SnowSceneDisplayItem diamond() {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    item.element_id = {1, 1};
    item.rect_shape = SNOW_DISPLAY_RECT_SHAPE_DIAMOND;
    item.width = 80;
    item.height = 80;
    item.fill = {0, 0, 255, 255};
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.opacity = 1;
    item.corner_radii = {16, 0, 0, 0};
    item.magnifier = {0, 0, 40, 40, 2};
    return item;
}

void geometryRoundsOnlyTheRequestedTipAndClampsToCircularFillets() {
    const QRectF rect(-40, -40, 80, 80);
    const auto path = snow_canvas_render_geometry::roundedDiamondPath(rect, {16, 0, 0, 0});
    const double tip = -40 + 16 * (std::sqrt(2.0) - 1.0);
    require(!path.contains(QPointF(0, tip - 1)) && path.contains(QPointF(0, tip + 1)),
            "top radius must form a tangent circular fillet");
    require(path.contains(QPointF(38, 0)) && path.contains(QPointF(0, 38)) &&
                path.contains(QPointF(-38, 0)),
            "independent top rounding must preserve the other tips");
    for (const QSizeF size : {QSizeF(80, 80), QSizeF(120, 60), QSizeF(60, 120)}) {
        const QRectF bounds(-size.width() / 2, -size.height() / 2, size.width(), size.height());
        const auto maximum =
            snow_canvas_render_geometry::roundedDiamondPath(bounds, {1000, 1000, 1000, 1000});
        const double a = size.width() / 2;
        const double b = size.height() / 2;
        const double radius = a * b / std::hypot(a, b);
        require(maximum.contains(QPointF(radius - 1, 0)) &&
                    !maximum.contains(QPointF(radius + 1, 0)),
                "maximum uniform rounding must meet at the diamond incircle");
        require(bounds.adjusted(-0.01, -0.01, 0.01, 0.01).contains(maximum.boundingRect()),
                "oversized rounding must remain within the original bounds");
    }
    require(snow_canvas_render_geometry::roundedDiamondPath(QRectF(0, 0, 0, 10), {6, 6, 6, 6})
                .isEmpty(),
            "degenerate diamonds must have no contour");
}

void fillStrokeAndPatternUseTheSameRoundedContour() {
    auto item = diamond();
    const QImage filled = renderShape(item);
    require(pixel(filled, 100, 61).alpha() == 0 && pixel(filled, 100, 70) == QColor(Qt::blue),
            "rounded shape fills must remove the original tip and retain the interior");
    item.stroke = {255, 0, 0, 255};
    item.stroke_width = 2;
    const QImage outlined = renderShape(item);
    require(pixel(outlined, 100, 61).alpha() == 0 && pixel(outlined, 100, 66).red() > 0,
            "shape stroke must follow the same rounded arc as its fill");
    item.stroke.a = 0;
    item.corner_radii = {16, 16, 16, 16};
    for (const auto style : {SNOW_FILL_STYLE_LINE, SNOW_FILL_STYLE_CROSS_LINE}) {
        item.fill_style = style;
        const QImage patterned = renderShape(item);
        require(pixel(patterned, 100, 61).alpha() == 0,
                "patterned fill must stay inside the rounded diamond");
        int ink = 0;
        for (int y = 80; y < 120; ++y) {
            for (int x = 80; x < 120; ++x) {
                ink += patterned.pixelColor(x, y).alpha() > 0 ? 1 : 0;
            }
        }
        require(ink > 0 && ink < 1600, "rounded diamond hatching must contain ink and gaps");
    }
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.corner_radii = {};
    require(pixel(renderShape(item), 100, 61).alpha() > 0,
            "zero radii must retain the original sharp diamond tip");
}

void shapesSpotlightsAndMagnifiersAgreeUnderProjection() {
    for (double dpr : {1.0, 1.25, 2.0}) {
        for (double zoom : {0.5, 1.0, 2.0}) {
            for (double rotation : {0.0, 0.63}) {
                auto item = diamond();
                item.rotation = rotation;
                item.corner_radii = {16, 12, 8, 4};
                const QImage shape = renderShape(item, dpr, zoom);
                item.kind = SNOW_SCENE_DISPLAY_ITEM_MAGNIFIER;
                const QImage lens = renderShape(item, dpr, zoom);
                SnowSpotlightCutout cutout{};
                cutout.width = item.width;
                cutout.height = item.height;
                cutout.rotation = rotation;
                cutout.shape = SNOW_DISPLAY_RECT_SHAPE_DIAMOND;
                cutout.corner_radii = item.corner_radii;
                QImage mask(shape.size(), QImage::Format_ARGB32_Premultiplied);
                mask.setDevicePixelRatio(dpr);
                mask.fill(Qt::white);
                QPainter painter(&mask);
                snow_canvas_spotlight_renderer::render(
                    painter, projection(zoom), SpotlightDisplayInfo{{0, 0, 0, 255}, 0.64, true},
                    &cutout, 1, QRectF(0, 0, 200, 200), QRegion(QRect(0, 0, 200, 200)));
                painter.end();
                QTransform transform;
                transform.translate(100, 100);
                transform.scale(zoom, zoom);
                transform.rotateRadians(rotation);
                for (QPointF local : {QPointF(0, 0), QPointF(0, -39), QPointF(0, -25),
                                      QPointF(39, 0), QPointF(25, 0), QPointF(30, 30)}) {
                    const QPointF view = transform.map(local);
                    const bool inside = local == QPointF(0, 0) || local == QPointF(0, -25) ||
                                        local == QPointF(25, 0);
                    require((pixel(shape, view.x(), view.y()).alpha() > 200) == inside &&
                                (pixel(lens, view.x(), view.y()).alpha() > 200) == inside,
                            "shape and lens clipping must agree at every zoom, rotation and DPR");
                    require((pixel(mask, view.x(), view.y()).red() > 240) == inside,
                            "spotlight cutouts must use the same rounded diamond contour");
                }
            }
        }
    }
}

void overlappingRoundedDiamondCutoutsRevealTheirUnion() {
    SnowSpotlightCutout cutouts[2]{};
    cutouts[0].width = 80;
    cutouts[0].height = 80;
    cutouts[0].shape = SNOW_DISPLAY_RECT_SHAPE_DIAMOND;
    cutouts[0].corner_radii = {16, 16, 16, 16};
    cutouts[1] = cutouts[0];
    cutouts[1].width = 20;
    cutouts[1].height = 20;
    cutouts[1].center_y = -39;
    QImage mask(200, 200, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::white);
    QPainter painter(&mask);
    snow_canvas_spotlight_renderer::render(
        painter, projection(), SpotlightDisplayInfo{{0, 0, 0, 255}, 0.64, true}, cutouts, 2,
        QRectF(0, 0, 200, 200), QRegion(QRect(0, 0, 200, 200)));
    painter.end();
    require(mask.pixelColor(100, 61) == QColor(Qt::white),
            "another rounded diamond hole must reveal the area removed by a rounded tip");
    require(mask.pixelColor(140, 61).red() < 100,
            "rounded diamond unions must leave the surrounding mask intact");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    geometryRoundsOnlyTheRequestedTipAndClampsToCircularFillets();
    fillStrokeAndPatternUseTheSameRoundedContour();
    shapesSpotlightsAndMagnifiersAgreeUnderProjection();
    overlappingRoundedDiamondCutoutsRevealTheirUnion();
    return 0;
}

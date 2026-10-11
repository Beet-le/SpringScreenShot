#include "snow_canvas_magnifier_renderer.h"

#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QPainter>
#include <QPaintEngine>
#include <QTransform>

#include <cmath>
#include <limits>
#include <utility>

namespace snow_canvas_magnifier_renderer {
namespace {

thread_local RenderDiagnostics g_magnifierDiagnostics;

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.x()) && std::isfinite(rect.y()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

struct SourceLayer {
    const SnowCanvasBaseImageSource* source;
    QPainterPath coveragePath;
};

struct SamplingPlan {
    QList<SourceLayer> exposedLayers;
    bool usesGroupMask = false;
};

SamplingPlan samplingPlan(const QList<SnowCanvasBaseImageSource>& sources,
                          const QTransform& sourceToView, const QTransform& viewToDevice,
                          const QRectF& lensBounds, const QRectF& visible) {
    SamplingPlan plan;
    plan.exposedLayers.reserve(sources.size());
    const QTransform viewToSource = sourceToView.inverted();
    const QRectF lensSamples = viewToSource.mapRect(lensBounds);
    const QRectF exposedSamples = viewToSource.mapRect(visible);
    const QRectF devicePixel =
        (sourceToView * viewToDevice).inverted().mapRect(QRectF(0.0, 0.0, 1.0, 1.0));
    bool hasLensLayer = false;
    for (const auto& source : sources) {
        if (source.image.isNull() || !finiteRect(source.canvasRect) ||
            !source.canvasRect.isValid() || !finiteRect(source.coverage))
            continue;
        const QRectF coverage = source.coverage.isEmpty()
                                    ? source.canvasRect
                                    : source.canvasRect.intersected(source.coverage);
        // Keep culling conservative at interpolation and device clip boundaries.
        const double guardX =
            qMax(source.canvasRect.width() / source.image.width(), devicePixel.width());
        const double guardY =
            qMax(source.canvasRect.height() / source.image.height(), devicePixel.height());
        if (!lensSamples.adjusted(-guardX, -guardY, guardX, guardY).intersects(coverage))
            continue;
        // Masking belongs to the complete lens. Exposure may cull a layer, but
        // must not switch the remaining pixels between direct and group masks.
        plan.usesGroupMask |= hasLensLayer;
        hasLensLayer = true;
        if (!exposedSamples.adjusted(-guardX, -guardY, guardX, guardY).intersects(coverage))
            continue;
        SourceLayer layer{&source, {}};
        if (!source.coverage.isEmpty()) {
            QPainterPath coveragePath;
            coveragePath.addRect(coverage);
            layer.coveragePath = sourceToView.map(coveragePath);
        }
        plan.exposedLayers.append(std::move(layer));
    }
    return plan;
}

void drawLayer(QPainter& painter, const SourceLayer& layer, const QTransform& sourceToView,
               const QPainterPath* lensClip = nullptr) {
    painter.save();
    if (lensClip != nullptr) {
        // Resolve geometric coverage before raster clipping so partial exposure
        // cannot change rectangle-versus-path edge rounding.
        const QPainterPath clip =
            layer.coveragePath.isEmpty() ? *lensClip : lensClip->intersected(layer.coveragePath);
        painter.setClipPath(clip, Qt::IntersectClip);
    } else if (!layer.coveragePath.isEmpty()) {
        painter.setClipPath(layer.coveragePath, Qt::IntersectClip);
    }
    // Keep the complete image-to-canvas transform. Cropping a source rectangle
    // would clamp bilinear neighbors independently at each exposure or tile edge.
    painter.setWorldTransform(sourceToView, true);
    painter.drawImage(layer.source->canvasRect, layer.source->image,
                      QRectF(layer.source->image.rect()));
    painter.restore();
    ++g_magnifierDiagnostics.sourceLayerDrawCount;
}

void drawCompositedLayers(QPainter& painter, const QList<SourceLayer>& layers,
                          const QTransform& sourceToView, const QPainterPath& viewPath,
                          const QRectF& visible) {
    const QTransform viewToDevice = painter.deviceTransform();
    QRectF deviceBounds = viewToDevice.mapRect(visible);
    // Align both edges to the physical pixel grid, including export scale, DPR,
    // and enclosing filter-tile translations. Device metrics are not uniformly
    // physical (QWidget uses logical dimensions). A raster engine's paintDevice
    // is its physical backing image, including redirected widget/pixmap paints.
    const auto* engine = painter.paintEngine();
    QRegion systemClip;
    if (engine != nullptr && engine->type() == QPaintEngine::Raster) {
        if (const auto* target = engine->paintDevice())
            deviceBounds =
                deviceBounds.intersected(QRectF(0, 0, target->width(), target->height()));
        systemClip = engine->systemClip();
        if (!systemClip.isEmpty())
            deviceBounds = deviceBounds.intersected(QRectF(systemClip.boundingRect()));
    }
    constexpr double minCoordinate = std::numeric_limits<int>::min() + 1.0;
    constexpr double maxCoordinate = std::numeric_limits<int>::max() - 1.0;
    constexpr double maxExtent = std::numeric_limits<int>::max() - 2.0;
    if (!finiteRect(deviceBounds) || deviceBounds.isEmpty() ||
        deviceBounds.left() < minCoordinate || deviceBounds.top() < minCoordinate ||
        deviceBounds.right() > maxCoordinate || deviceBounds.bottom() > maxCoordinate ||
        deviceBounds.width() > maxExtent || deviceBounds.height() > maxExtent)
        return;
    const QRect pixels = deviceBounds.toAlignedRect();
    QPainterPath deviceClip = viewToDevice.map(viewPath);
    if (painter.hasClipping())
        deviceClip = deviceClip.intersected(viewToDevice.map(painter.clipPath()));
    if (!systemClip.isEmpty()) {
        QPainterPath systemPath;
        systemPath.addRegion(systemClip);
        deviceClip = deviceClip.intersected(systemPath);
    }
    constexpr int stripeMaxHeight = snow_canvas_filter_tile_cache::kTilePhysicalSize;
    constexpr int scratchPixelBudget = stripeMaxHeight * stripeMaxHeight;
    // Qt's downsampling rasterizer uses different precision for SIMD batches
    // and their scalar tails. Splitting a horizontal span introduces new tails
    // and visible seams, even with the complete original source rectangle.
    // Full-width stripes preserve each source/coverage span's actual endpoints.
    // Bound their height by a fixed pixel budget, with one full row as the minimum.
    const int stripeHeight =
        qMin(pixels.height(), qMin(stripeMaxHeight, qMax(1, scratchPixelBudget / pixels.width())));
    QImage scratch = snowCanvasAllocateImage(QSize(pixels.width(), stripeHeight),
                                             QImage::Format_ARGB32_Premultiplied);
    if (scratch.isNull())
        return;
    const auto scratchPixels =
        static_cast<std::size_t>(scratch.width()) * static_cast<std::size_t>(scratch.height());
    g_magnifierDiagnostics.peakCompositingPixelCount =
        qMax(g_magnifierDiagnostics.peakCompositingPixelCount, scratchPixels);
    for (qint64 top = pixels.top(); top <= pixels.bottom(); top += stripeHeight) {
        const QRect tile(pixels.left(), static_cast<int>(top), pixels.width(),
                         qMin(stripeHeight, static_cast<int>(pixels.bottom() - top + 1)));
        if (!deviceClip.intersects(QRectF(tile).adjusted(-1, -1, 1, 1)))
            continue;
        scratch.fill(Qt::transparent);
        {
            QPainter layersPainter(&scratch);
            snow_canvas_renderer::copyPainterFontAndHints(layersPainter, painter);
            layersPainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            layersPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            layersPainter.setClipRect(QRect(0, 0, tile.width(), tile.height()));
            layersPainter.setWorldTransform(
                viewToDevice * QTransform::fromTranslate(-static_cast<double>(pixels.left()),
                                                         -static_cast<double>(top)));
            for (const auto& layer : layers)
                drawLayer(layersPainter, layer, sourceToView);
        }
        painter.save();
        // The lens is a group mask. Apply it and the existing exposure clip
        // once after composing originals at full opacity with SourceOver.
        painter.setClipPath(viewPath, Qt::IntersectClip);
        painter.setWorldTransform(QTransform());
        painter.setWorldTransform(painter.deviceTransform().inverted());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(QRectF(tile), scratch, QRectF(0, 0, tile.width(), tile.height()));
        painter.restore();
        ++g_magnifierDiagnostics.compositedTileCount;
        g_magnifierDiagnostics.compositedPixelCount +=
            static_cast<std::size_t>(tile.width()) * static_cast<std::size_t>(tile.height());
    }
}

} // namespace

RenderDiagnostics diagnosticsForCurrentThread() {
    return g_magnifierDiagnostics;
}

void resetDiagnosticsForCurrentThread() {
    g_magnifierDiagnostics = {};
}

QPainterPath lensPath(const SnowSceneDisplayItem& item) {
    QPainterPath path;
    const QRectF rect(-item.width / 2.0, -item.height / 2.0, item.width, item.height);
    if (!finiteRect(rect) || !rect.isValid() || !std::isfinite(item.rotation) ||
        !std::isfinite(item.center_x) || !std::isfinite(item.center_y))
        return path;
    if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        path.addEllipse(rect);
    } else if (item.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        path = snow_canvas_render_geometry::roundedDiamondPath(rect, item.corner_radii);
    } else {
        path = snow_canvas_render_geometry::roundedRectPath(
            rect, snow_canvas_render_geometry::toViewCornerRadii(item.corner_radii, 1.0, rect));
    }
    QTransform element;
    element.translate(item.center_x, item.center_y);
    element.rotateRadians(item.rotation);
    return element.map(path);
}

void render(QPainter& painter, const SceneDisplayInfo& displayInfo,
            const SnowSceneDisplayItem& item, const QList<SnowCanvasBaseImageSource>* sources) {
    const double zoom = displayInfo.camera_zoom;
    if (!std::isfinite(zoom) || zoom <= 0.0)
        return;
    const QPainterPath canvasPath = lensPath(item);
    if (canvasPath.isEmpty())
        return;
    const QTransform canvasToView(
        zoom, 0.0, 0.0, zoom, displayInfo.surface_width / 2.0 - displayInfo.camera_center_x * zoom,
        displayInfo.surface_height / 2.0 - displayInfo.camera_center_y * zoom);
    const QPainterPath viewPath = canvasToView.map(canvasPath);
    const QRectF exposed = painter.hasClipping() ? painter.clipBoundingRect()
                                                 : QRectF(0.0, 0.0, displayInfo.surface_width,
                                                          displayInfo.surface_height);
    const QRectF visible = viewPath.boundingRect().intersected(exposed);
    const double strokeWidth = item.stroke_width * zoom;
    const bool hasStroke = item.stroke.a != 0 && std::isfinite(strokeWidth) && strokeWidth > 0.0;
    // Source sampling needs the interior; the border can be exposed outside it.
    if (visible.isEmpty() && !hasStroke)
        return;
    const double opacity = painter.opacity() * qBound(0.0, item.opacity, 1.0);
    if (opacity == 0.0)
        return;
    ++g_magnifierDiagnostics.renderedMagnifierCount;
    painter.save();
    painter.setOpacity(opacity);
    const auto& geometry = item.magnifier;
    const double factor = geometry.magnification_factor;
    if (!visible.isEmpty() && sources != nullptr && std::isfinite(factor) && factor >= 1.0 &&
        factor <= 10.0 && std::isfinite(geometry.source_center_x) &&
        std::isfinite(geometry.source_center_y)) {
        const double scale = factor * zoom;
        const QPointF destinationCenter = canvasToView.map(QPointF(item.center_x, item.center_y));
        const QTransform sourceToView(scale, 0.0, 0.0, scale,
                                      destinationCenter.x() - geometry.source_center_x * scale,
                                      destinationCenter.y() - geometry.source_center_y * scale);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const auto plan = samplingPlan(*sources, sourceToView, painter.deviceTransform(),
                                       viewPath.boundingRect(), visible);
        if (!plan.exposedLayers.isEmpty()) {
            if (plan.usesGroupMask) {
                drawCompositedLayers(painter, plan.exposedLayers, sourceToView, viewPath, visible);
            } else {
                for (const auto& layer : plan.exposedLayers)
                    drawLayer(painter, layer, sourceToView, &viewPath);
            }
        }
        painter.restore();
    }
    if (hasStroke) {
        painter.setPen(QPen(snow_canvas_renderer::toQColor(item.stroke), strokeWidth, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(viewPath);
    }
    painter.restore();
}

} // namespace snow_canvas_magnifier_renderer

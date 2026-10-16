#pragma once

#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_draw_engine_qt/snow_canvas_export_types.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QColorSpace>
#include <QPainter>
#include <functional>

namespace screenshot_selection_background {
struct Span {
    qint64 sourceBegin;
    qint64 sourceEnd;
    int destinationBegin;
    int destinationEnd;
};

// Occupied bands, including shadow footprints, keep their native scale. Only
// empty intervals are resampled. Keep the two axes separate: sparse selections
// must never allocate a Cartesian tile list or an uncompressed source raster.
inline QVector<Span> spans(const QVector<screenshot_layout_compaction::Band>& bands,
                           int sourceExtent, int padding) {
    if (bands.isEmpty())
        return {{-padding, qint64(sourceExtent) + padding, 0, sourceExtent + 2 * padding}};
    QVector<Span> result;
    result.reserve(bands.size() * 2 + 1);
    if (padding > 0)
        result.append({-padding, 0, 0, padding});
    int sourceEnd = 0;
    int destinationEnd = 0;
    for (const auto& band : bands) {
        if (band.begin > sourceEnd && band.destination > destinationEnd)
            result.append(
                {sourceEnd, band.begin, destinationEnd + padding, band.destination + padding});
        sourceEnd = band.end;
        destinationEnd = band.destination + band.end - band.begin;
        result.append({band.begin, band.end, band.destination + padding, destinationEnd + padding});
    }
    if (padding > 0)
        result.append({sourceExtent, qint64(sourceExtent) + padding, destinationEnd + padding,
                       destinationEnd + 2 * padding});
    return result;
}
} // namespace screenshot_selection_background

// Returns capture pixels aligned to the complete result, including its effect
// insets. Annotations are deliberately absent; they belong to the selected layer.
inline QImage renderScreenshotSelectionBackground(
    const QList<CanvasExportSource>& sources, const QRectF& selection, const QSize& sourcePixelSize,
    const ScreenshotResultStyle& outputStyle, const ScreenshotLayoutCompactionPlan* plan = nullptr,
    const std::function<bool()>& cancelled = {}) {
    if (outputStyle.backgroundOpacityPercent <= 0 || selection.isEmpty() ||
        sourcePixelSize.isEmpty() || (plan && !plan->isValid()))
        return {};
    const QSize contentSize = plan ? plan->outputSize : sourcePixelSize;
    const int effect = ScreenshotResultCompositor::normalizedStyle(outputStyle).shadowWidth;
    const qint64 width = qint64(contentSize.width()) + 2 * effect;
    const qint64 height = qint64(contentSize.height()) + 2 * effect;
    if (width <= 0 || height <= 0 || width > 100000000 / height || !std::isfinite(selection.x()) ||
        !std::isfinite(selection.y()) || !std::isfinite(selection.width()) ||
        !std::isfinite(selection.height()) || (plan && plan->sourceSize != sourcePixelSize) ||
        (cancelled && cancelled()))
        return {};
    const auto layout = ScreenshotResultCompositor::layoutForContent(contentSize, outputStyle);
    if (!layout.isValid() ||
        qint64(layout.outputRect.width()) * layout.outputRect.height() > 100000000)
        return {};
    QImage result = snowCanvasAllocateZeroedImage(layout.outputRect.size(),
                                                  QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return {};
    result.setColorSpace(QColorSpace::SRgb);
    const int padding = layout.effectInsets.left();
    using namespace screenshot_selection_background;
    const auto columns =
        spans(plan && plan->changed ? plan->columns : QVector<screenshot_layout_compaction::Band>{},
              sourcePixelSize.width(), padding);
    const auto rows =
        spans(plan && plan->changed ? plan->rows : QVector<screenshot_layout_compaction::Band>{},
              sourcePixelSize.height(), padding);
    const qreal sx = selection.width() / sourcePixelSize.width();
    const qreal sy = selection.height() / sourcePixelSize.height();
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto& row : rows) {
        for (const auto& column : columns) {
            if (cancelled && cancelled())
                return {};
            const QRectF source(selection.x() + static_cast<qreal>(column.sourceBegin) * sx,
                                selection.y() + static_cast<qreal>(row.sourceBegin) * sy,
                                static_cast<qreal>(column.sourceEnd - column.sourceBegin) * sx,
                                static_cast<qreal>(row.sourceEnd - row.sourceBegin) * sy);
            const QRectF destination(column.destinationBegin, row.destinationBegin,
                                     column.destinationEnd - column.destinationBegin,
                                     row.destinationEnd - row.destinationBegin);
            painter.setClipRect(destination);
            for (const auto& layer : sources) {
                if (layer.image.isNull() || layer.canvasRect.isEmpty())
                    continue;
                const QRectF intersection = source.intersected(layer.canvasRect);
                if (intersection.isEmpty())
                    continue;
                const QRectF from(
                    (intersection.x() - layer.canvasRect.x()) * layer.image.width() /
                        layer.canvasRect.width(),
                    (intersection.y() - layer.canvasRect.y()) * layer.image.height() /
                        layer.canvasRect.height(),
                    intersection.width() * layer.image.width() / layer.canvasRect.width(),
                    intersection.height() * layer.image.height() / layer.canvasRect.height());
                const QRectF to(destination.x() + (intersection.x() - source.x()) *
                                                      destination.width() / source.width(),
                                destination.y() + (intersection.y() - source.y()) *
                                                      destination.height() / source.height(),
                                intersection.width() * destination.width() / source.width(),
                                intersection.height() * destination.height() / source.height());
                painter.drawImage(to, layer.image, from);
            }
        }
    }
    return result;
}

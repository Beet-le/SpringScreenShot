// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include <QPainter>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

namespace snow_shot::presentation {
ImageEditingCanvas::ImageEditingCanvas(ImageEditingDocument& document, QWidget* parent)
    : SnowCanvasWidget(document.runtime(), parent), m_document(document) {
    setObjectName(QStringLiteral("imageEditingCanvas"));
    setCustomRenderer(this);
    setClearBackgroundEnabled(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setWheelZoomEnabled(false);
    setMouseTracking(true);
    setMode(Mode::Pan);
    connect(&document, &ImageEditingDocument::baseImageChanged, this, [this] { syncImage(); });
}
ImageEditingCanvas::~ImageEditingCanvas() {
    setCustomRenderer(nullptr);
}
void ImageEditingCanvas::setMode(Mode mode) {
    m_cropping = m_panning = false;
    m_mode = mode;
    setInteractionEnabled(mode == Mode::Draw);
    setCursor(mode == Mode::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
}
void ImageEditingCanvas::syncImage() {
    setContentClip(m_document.bounds());
    setDecorationRenderAreas({m_document.bounds(), m_document.bounds()});
    setBaseImageSources(baseImageSources());
    if (!m_fitted)
        fit();
    else
        syncCamera();
    update();
}
void ImageEditingCanvas::fit() {
    const auto bounds = m_document.bounds();
    if (bounds.isEmpty() || width() <= 0 || height() <= 0)
        return;
    m_center = bounds.center();
    m_zoom = std::min(std::max(1, width() - 48) / bounds.width(),
                      std::max(1, height() - 48) / bounds.height());
    m_fitted = true;
    syncCamera();
}
void ImageEditingCanvas::actualSize() {
    if (m_document.bounds().isEmpty())
        return;
    m_zoom = m_document.image().width() / m_document.bounds().width() / devicePixelRatioF();
    syncCamera();
}
void ImageEditingCanvas::syncCamera() {
    setViewportCamera(m_center.x(), m_center.y(), m_zoom);
}
QPointF ImageEditingCanvas::pixelAt(QPointF view) const {
    return m_document.imagePixelAt(canvasToViewTransform().inverted().map(view));
}
void ImageEditingCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton ||
        (m_mode == Mode::Pan && event->button() == Qt::LeftButton)) {
        m_panning = true;
        m_last = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        (m_mode == Mode::CropRows || m_mode == Mode::CropColumns)) {
        if (!m_document.bounds().contains(
                canvasToViewTransform().inverted().map(event->position()))) {
            event->accept();
            return;
        }
        m_cropping = true;
        m_firstPixel = m_lastPixel = pixelAt(event->position());
        event->accept();
        update(cropPreviewDamage());
        return;
    }
    SnowCanvasWidget::mousePressEvent(event);
}
void ImageEditingCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) {
        m_center -= (event->position() - m_last) / m_zoom;
        m_last = event->position();
        syncCamera();
        event->accept();
        return;
    }
    if (m_cropping) {
        const auto previous = cropPreviewDamage();
        m_lastPixel = pixelAt(event->position());
        update(QRegion(previous) | cropPreviewDamage());
        event->accept();
        return;
    }
    SnowCanvasWidget::mouseMoveEvent(event);
}
void ImageEditingCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (m_panning) {
        m_panning = false;
        setCursor(m_mode == Mode::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
        event->accept();
        return;
    }
    if (m_cropping && event->button() == Qt::LeftButton) {
        const auto previous = cropPreviewDamage();
        m_cropping = false;
        m_lastPixel = pixelAt(event->position());
        const bool rows = m_mode == Mode::CropRows;
        if (cropRequested)
            cropRequested(rows ? ImageEditingDocument::CropAxis::Rows
                               : ImageEditingDocument::CropAxis::Columns,
                          qRound(rows ? m_firstPixel.y() : m_firstPixel.x()),
                          qRound(rows ? m_lastPixel.y() : m_lastPixel.x()));
        update(previous);
        event->accept();
        return;
    }
    SnowCanvasWidget::mouseReleaseEvent(event);
}
void ImageEditingCanvas::wheelEvent(QWheelEvent* event) {
    if (hasActiveTextEditing()) {
        SnowCanvasWidget::wheelEvent(event);
        return;
    }
    if (event->modifiers() != Qt::NoModifier && styleWheel && styleWheel(event))
        return;
    const auto anchor = canvasToViewTransform().inverted().map(event->position());
    const double steps = event->pixelDelta().isNull() ? event->angleDelta().y() / 120.0
                                                      : event->pixelDelta().y() / 50.0;
    m_zoom = std::clamp(m_zoom * std::pow(1.15, steps), 1e-9, 1e6);
    m_center = anchor - (event->position() - QPointF(width() / 2.0, height() / 2.0)) / m_zoom;
    syncCamera();
    event->accept();
}
void ImageEditingCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && m_cropping) {
        const auto previous = cropPreviewDamage();
        m_cropping = false;
        update(previous);
        event->accept();
        return;
    }
    if (!hasActiveTextEditing() &&
        (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo))) {
        const bool redo = event->matches(QKeySequence::Redo);
        if (historyRequested)
            historyRequested(redo);
        else if (redo)
            m_document.redo();
        else
            m_document.undo();
        event->accept();
        return;
    }
    SnowCanvasWidget::keyPressEvent(event);
}
void ImageEditingCanvas::resizeEvent(QResizeEvent* event) {
    SnowCanvasWidget::resizeEvent(event);
    if (!m_fitted)
        fit();
    else
        syncCamera();
}
std::uint64_t ImageEditingCanvas::contentRevision() const {
    return static_cast<std::uint64_t>(m_document.image().cacheKey());
}
QList<SnowCanvasBaseImageSource> ImageEditingCanvas::baseImageSources() const {
    return m_document.baseImageSources();
}
std::optional<SnowCanvasFilterRenderReference> ImageEditingCanvas::filterRenderReference() const {
    if (m_document.bounds().isEmpty())
        return {};
    return SnowCanvasFilterRenderReference{m_document.bounds(), m_document.image().width() /
                                                                    m_document.bounds().width()};
}
void ImageEditingCanvas::renderBeforeCanvas(QPainter& painter,
                                            const SnowCanvasRenderContext& context) {
    painter.setTransform(context.canvasToViewTransform);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(m_document.bounds(), m_document.image());
}
void ImageEditingCanvas::renderOriginalBackground(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    renderBeforeCanvas(painter, context);
}
QRectF ImageEditingCanvas::cropStrip(int* pixelCount) const {
    const auto bounds = m_document.bounds();
    const bool rows = m_mode == Mode::CropRows;
    const int extent = rows ? m_document.image().height() : m_document.image().width();
    if (extent == 0)
        return {};
    const int first = std::clamp(qRound(rows ? m_firstPixel.y() : m_firstPixel.x()), 0, extent);
    const int last = std::clamp(qRound(rows ? m_lastPixel.y() : m_lastPixel.x()), 0, extent);
    const double scale = (rows ? bounds.height() : bounds.width()) / extent;
    if (pixelCount)
        *pixelCount = std::abs(first - last);
    return rows ? QRectF(bounds.x(), bounds.y() + std::min(first, last) * scale, bounds.width(),
                         std::abs(first - last) * scale)
                : QRectF(bounds.x() + std::min(first, last) * scale, bounds.y(),
                         std::abs(first - last) * scale, bounds.height());
}
QRect ImageEditingCanvas::cropPreviewDamage() const {
    if (!m_cropping || cropStrip().isEmpty())
        return {};
    return canvasToViewTransform()
        .mapRect(cropStrip())
        .toAlignedRect()
        .adjusted(-2, -2, 2, 2)
        .intersected(rect());
}
void ImageEditingCanvas::renderAfterCanvas(QPainter& painter,
                                           const SnowCanvasRenderContext& context) {
    if (!m_cropping)
        return;
    int pixels = 0;
    const QRectF view = context.canvasToViewTransform.mapRect(cropStrip(&pixels));
    if (view.isEmpty())
        return;
    painter.fillRect(view, QColor(240, 65, 65, 95));
    painter.setPen(Qt::white);
    painter.drawText(view, Qt::AlignCenter,
                     QCoreApplication::translate("ImageEditingWindow", "%1 px").arg(pixels));
}
} // namespace snow_shot::presentation

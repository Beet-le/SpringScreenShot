// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QSet>
#include <cstring>
#include <QPainter>
#include <algorithm>

namespace snow_shot::presentation {
namespace {
QList<SnowCanvasBaseImageSource>
exteriorSamplingSources(const QList<SnowCanvasBaseImageSource>& sources, const QRectF& bounds) {
    QList<SnowCanvasBaseImageSource> result;
    for (const auto& source : sources) {
        if (source.image.isNull() || source.canvasRect.isEmpty())
            continue;
        const auto coverage = source.coverage.isEmpty()
                                  ? source.canvasRect
                                  : source.canvasRect.intersected(source.coverage);
        const auto overlap = coverage.intersected(bounds);
        if (overlap.isEmpty()) {
            if (!coverage.isEmpty())
                result.append({source.image, source.canvasRect, coverage, source.smartEraseSource});
            continue;
        }
        // Keep only immutable context outside the editable image. In particular, transparent
        // pixels and strips removed by later crops must never reveal the original image below.
        const QRectF pieces[]{
            {coverage.left(), coverage.top(), coverage.width(), overlap.top() - coverage.top()},
            {coverage.left(), overlap.bottom(), coverage.width(),
             coverage.bottom() - overlap.bottom()},
            {coverage.left(), overlap.top(), overlap.left() - coverage.left(), overlap.height()},
            {overlap.right(), overlap.top(), coverage.right() - overlap.right(), overlap.height()}};
        for (const auto& piece : pieces)
            if (!piece.isEmpty())
                result.append({source.image, source.canvasRect, piece, source.smartEraseSource});
    }
    return result;
}

QImage joinStrip(const QImage& current, const ImageEditingDocument::Crop& crop,
                 const std::function<bool()>& cancelled) {
    if (cancelled && cancelled())
        return {};
    QImage restored = snowCanvasAllocateImage(crop.beforeSize, current.format());
    if (restored.isNull())
        return {};
    restored.setColorSpace(current.colorSpace());
    restored.setColorTable(current.colorTable());
    const qsizetype pixelBytes = current.depth() / 8;
    for (int y = 0; y < restored.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled())
            return {};
        uchar* destination = restored.scanLine(y);
        if (crop.axis == ImageEditingDocument::CropAxis::Rows) {
            const uchar* source = y < crop.start ? current.constScanLine(y)
                                  : y < crop.start + crop.length
                                      ? crop.removed.constScanLine(y - crop.start)
                                      : current.constScanLine(y - crop.length);
            std::memcpy(destination, source, restored.width() * pixelBytes);
        } else {
            const uchar* source = current.constScanLine(y);
            std::memcpy(destination, source, crop.start * pixelBytes);
            std::memcpy(destination + crop.start * pixelBytes, crop.removed.constScanLine(y),
                        crop.length * pixelBytes);
            std::memcpy(destination + (crop.start + crop.length) * pixelBytes,
                        source + crop.start * pixelBytes,
                        (current.width() - crop.start) * pixelBytes);
        }
    }
    return restored;
}
} // namespace

ImageEditingDocument::ImageEditingDocument(QObject* parent)
    : QObject(parent), m_runtime(SnowCanvasRuntimeConfig{screenshotCanvasStyleDefaults()}) {
    m_runtime.setDocumentChangedHandler([this] {
        pruneCrops();
        emit changed();
    });
}

bool ImageEditingDocument::load(const ImageEditingSource& source) {
    if (source.image.isNull() || !source.canvasRect.isValid() || source.resultStyle)
        return false;
    SnowCanvasRuntime replacement(SnowCanvasRuntimeConfig{screenshotCanvasStyleDefaults()});
    if (!source.canvasSession.isEmpty() &&
        !replacement.restoreDocumentSession(source.canvasSession))
        return false;
    if (source.canvasSession.isEmpty() && !source.canvasHistory.isEmpty() &&
        !replacement.restoreDocumentHistory(source.canvasHistory))
        return false;
    QImage image = source.image.depth() < 8 ? source.image.convertToFormat(QImage::Format_ARGB32)
                                            : source.image;
    if (image.isNull() || !m_runtime.cloneDocumentSessionFrom(replacement))
        return false;
    m_image = std::move(image);
    if (m_image.devicePixelRatio() != 1)
        m_image.setDevicePixelRatio(1);
    m_originalFileName = source.originalFileName;
    m_origin = source.canvasRect.topLeft();
    m_unitsPerPixel = QSizeF(source.canvasRect.width() / m_image.width(),
                             source.canvasRect.height() / m_image.height());
    m_samplingSources = exteriorSamplingSources(source.samplingSources, source.canvasRect);
    m_crops.clear();
    m_serial = 0;
    updateSources();
    m_runtime.seedSmartEraseSnapshot(source.smartErase);
    m_saved = checkpoint();
    emit baseImageChanged();
    emit changed();
    return true;
}

QRectF ImageEditingDocument::bounds() const {
    return QRectF(m_origin, QSizeF(m_image.width() * m_unitsPerPixel.width(),
                                   m_image.height() * m_unitsPerPixel.height()));
}
QPointF ImageEditingDocument::imagePixelAt(QPointF point) const {
    return {(point.x() - m_origin.x()) / m_unitsPerPixel.width(),
            (point.y() - m_origin.y()) / m_unitsPerPixel.height()};
}

ImageEditingDocument::Crop
ImageEditingDocument::prepareCrop(const QImage& image, CropAxis axis, int first, int last,
                                  const std::function<bool()>& cancelled) {
    Crop crop;
    crop.axis = axis;
    crop.sourceKey = image.cacheKey();
    crop.beforeSize = image.size();
    const int extent = axis == CropAxis::Rows ? image.height() : image.width();
    first = std::clamp(first, 0, extent);
    last = std::clamp(last, 0, extent);
    crop.start = std::min(first, last);
    crop.length = std::abs(first - last);
    if (image.isNull() || crop.length == 0 || crop.length >= extent || (cancelled && cancelled()))
        return crop;
    const bool rows = axis == CropAxis::Rows;
    const QSize size(image.width() - (rows ? 0 : crop.length),
                     image.height() - (rows ? crop.length : 0));
    crop.result = snowCanvasAllocateImage(size, image.format());
    if (crop.result.isNull())
        return crop;
    crop.removed = image.copy(rows ? QRect(0, crop.start, image.width(), crop.length)
                                   : QRect(crop.start, 0, crop.length, image.height()));
    if (crop.removed.isNull()) {
        crop.result = {};
        return crop;
    }
    crop.result.setColorSpace(image.colorSpace());
    crop.result.setColorTable(image.colorTable());
    const qsizetype pixelBytes = image.depth() / 8;
    if (pixelBytes == 0) {
        crop.result = {};
        crop.removed = {};
        return crop;
    }
    for (int y = 0; y < crop.result.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled()) {
            crop.result = {};
            crop.removed = {};
            return crop;
        }
        uchar* destination = crop.result.scanLine(y);
        if (rows) {
            std::memcpy(destination, image.constScanLine(y < crop.start ? y : y + crop.length),
                        crop.result.width() * pixelBytes);
        } else {
            const uchar* source = image.constScanLine(y);
            std::memcpy(destination, source, crop.start * pixelBytes);
            std::memcpy(destination + crop.start * pixelBytes,
                        source + (crop.start + crop.length) * pixelBytes,
                        (crop.result.width() - crop.start) * pixelBytes);
        }
    }
    return crop;
}

bool ImageEditingDocument::applyCrop(Crop crop) {
    if (crop.result.isNull() || crop.sourceKey != m_image.cacheKey())
        return false;
    const auto before = baseState();
    const auto token = ++m_serial;
    if (!m_runtime.commitHostHistoryState(token))
        return false;
    m_image = std::move(crop.result);
    m_crops.insert(token, CropEntry{before, std::move(crop)});
    pruneCrops();
    updateSources();
    emit baseImageChanged();
    emit changed();
    return true;
}
bool ImageEditingDocument::crop(CropAxis axis, int first, int last) {
    return applyCrop(prepareCrop(m_image, axis, first, last));
}
ImageEditingDocument::HistoryTransition ImageEditingDocument::historyTransition(bool redo) const {
    HistoryTransition transition;
    transition.redo = redo;
    transition.state = baseState();
    transition.nextState = m_runtime.hostHistoryState(redo ? 1 : -1);
    transition.revision = m_runtime.documentRevision();
    transition.sourceKey = m_image.cacheKey();
    if (transition.nextState != transition.state) {
        const auto entry = m_crops.constFind(redo ? transition.nextState : transition.state);
        if (entry != m_crops.cend()) {
            transition.image = m_image;
            transition.crop = entry->crop;
        }
    }
    return transition;
}
void ImageEditingDocument::prepareHistoryTransition(HistoryTransition& transition,
                                                    const std::function<bool()>& cancelled) {
    if (transition.image.isNull())
        return;
    transition.image =
        transition.redo ? prepareCrop(transition.image, transition.crop.axis, transition.crop.start,
                                      transition.crop.start + transition.crop.length, cancelled)
                              .result
                        : joinStrip(transition.image, transition.crop, cancelled);
}
bool ImageEditingDocument::applyHistoryTransition(HistoryTransition transition) {
    if (baseState() != transition.state || m_image.cacheKey() != transition.sourceKey ||
        m_runtime.documentRevision() != transition.revision ||
        m_runtime.hostHistoryState(transition.redo ? 1 : -1) != transition.nextState)
        return false;
    if (transition.state != transition.nextState && transition.image.isNull())
        return false;
    if (!(transition.redo ? m_runtime.redo() : m_runtime.undo()))
        return false;
    if (transition.state != transition.nextState) {
        m_image = std::move(transition.image);
        updateSources();
        emit baseImageChanged();
    }
    emit changed();
    return true;
}
bool ImageEditingDocument::travel(bool redo) {
    auto transition = historyTransition(redo);
    prepareHistoryTransition(transition);
    return applyHistoryTransition(std::move(transition));
}
void ImageEditingDocument::pruneCrops() {
    if (m_runtime.canRedo())
        return;
    QSet<quint64> retained;
    for (quint64 state = baseState(); state && m_crops.contains(state);
         state = m_crops.value(state).before)
        retained.insert(state);
    // A host entry is published just before its Qt delta is installed.
    if (baseState() && !m_crops.contains(baseState()))
        return;
    for (auto it = m_crops.begin(); it != m_crops.end();)
        if (!retained.contains(it.key()))
            it = m_crops.erase(it);
        else
            ++it;
}
bool ImageEditingDocument::undo() {
    return travel(false);
}
bool ImageEditingDocument::redo() {
    return travel(true);
}
void ImageEditingDocument::updateSources() {
    m_runtime.clearRenderState();
    m_runtime.setBaseImageSources(baseImageSources());
}
QList<SnowCanvasBaseImageSource> ImageEditingDocument::baseImageSources() const {
    auto sources = m_samplingSources;
    sources.append({m_image, bounds(), {}});
    return sources;
}
QByteArray ImageEditingDocument::checkpoint() const {
    QByteArray bytes = m_runtime.documentCheckpoint();
    if (bytes.isEmpty())
        return QByteArrayLiteral("unserializable:") +
               QByteArray::number(m_runtime.documentRevision()) + ":" +
               QByteArray::number(baseState());
    bytes += QByteArray::number(baseState());
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
bool ImageEditingDocument::dirty() const {
    return !m_image.isNull() && checkpoint() != m_saved;
}
void ImageEditingDocument::markExported(QByteArray checkpoint) {
    m_saved = std::move(checkpoint);
    emit changed();
}
ImageEditingSource ImageEditingDocument::snapshot() const {
    ImageEditingSource result;
    result.image = m_image;
    result.originalFileName = m_originalFileName;
    result.canvasRect = bounds();
    result.canvasSession = m_runtime.serializeDocumentSnapshot();
    if (result.canvasSession.isEmpty())
        result.error = QCoreApplication::translate(
            "ImageEditingWindow", "The editing content could not be prepared for export.");
    result.smartErase = m_runtime.smartEraseSnapshot();
    result.samplingSources = baseImageSources();
    return result;
}
} // namespace snow_shot::presentation

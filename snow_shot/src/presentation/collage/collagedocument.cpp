// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"

#include "collageresourcebudget_p.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snowimageqtcodec.h"

#include <QColorSpace>
#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace snow_shot::presentation {
namespace {
constexpr int previewExtent = 256;
constexpr qsizetype rowScratchBytes = 8 * 1024 * 1024;
constexpr qint64 conversionCacheBytes = 2 * 1024 * 1024;

QString resourceError() {
    return QCoreApplication::translate(
        "CollageDocument",
        "The collage exceeds the 8 GiB image limit. Try fewer images or smaller layout settings.");
}

QString preparationError() {
    return QCoreApplication::translate("CollageDocument",
                                       "The collage images could not be prepared for export.");
}

QString cancellationError() {
    return QCoreApplication::translate("CollageDocument", "The collage export was cancelled.");
}

void report(QString* destination, const QString& error) {
    if (destination)
        *destination = error;
}

struct CapturedImageLifetime final {
    std::shared_ptr<collage_detail::ResourceReservation> reservation;
    QImage image;
};

struct SourceLifetime final {
    std::shared_ptr<collage_detail::ResourceReservation> source;
    std::shared_ptr<collage_detail::ResourceReservation> preview;
};

std::shared_ptr<void> sourceLifetime(const QImage& image, const QImage& preview, QString* error) {
    try {
        auto owner = std::make_shared<SourceLifetime>();
        owner->source = collage_detail::reserve(image.sizeInBytes(), error);
        if (!owner->source)
            return {};
        owner->preview = collage_detail::reservePreview(preview.sizeInBytes(), error);
        return owner->preview ? owner : nullptr;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}

bool settingsValid(const CollageSettings& settings) {
    return settings.border >= 0 && settings.spacing >= 0 && settings.background.isValid() &&
           (settings.direction == CollageDirection::Vertical ||
            settings.direction == CollageDirection::Horizontal) &&
           (settings.sizing == CollageSizing::Original ||
            settings.sizing == CollageSizing::Uniform);
}

int itemIndex(const QVector<CollageItem>& items, quint64 id) {
    for (qsizetype index = 0; index < items.size(); ++index) {
        if (items[index].id == id)
            return static_cast<int>(index);
    }
    return -1;
}

QImage boundedPreview(const QImage& original, const QImage& supplied = {}) {
    QImage preview = supplied.isNull() ? original : supplied;
    if (preview.isNull())
        return {};
    if (preview.width() > previewExtent || preview.height() > previewExtent)
        preview = snowCanvasScaleImage(preview, QSize(previewExtent, previewExtent),
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (!preview.isNull() && preview.colorSpace().isValid() &&
        preview.colorSpace() != QColorSpace(QColorSpace::SRgb))
        preview = snowCanvasColorConvertedImage(preview, QColorSpace(QColorSpace::SRgb),
                                                QImage::Format_ARGB32_Premultiplied);
    if (!preview.isNull() && preview.devicePixelRatio() != 1)
        preview.setDevicePixelRatio(1);
    return preview;
}

QTransform canonicalTransform(const QTransform& transform) {
    return {static_cast<qreal>(qRound(transform.m11())),
            static_cast<qreal>(qRound(transform.m12())),
            static_cast<qreal>(qRound(transform.m21())),
            static_cast<qreal>(qRound(transform.m22())),
            0,
            0};
}

bool outputBytes(QSize size, qint64* bytes) {
    if (size.isEmpty() || size.width() > std::numeric_limits<int>::max() / 4)
        return false;
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels > collage_detail::maximumRasterBytes / 4)
        return false;
    *bytes = pixels * 4;
    return true;
}

bool imageIsOpaque(const QImage& image, const std::function<bool()>& cancelled) {
    if (!image.hasAlphaChannel())
        return true;
    const bool rgba = image.format() == QImage::Format_RGBA8888 ||
                      image.format() == QImage::Format_RGBA8888_Premultiplied;
    const bool argb = image.format() == QImage::Format_ARGB32 ||
                      image.format() == QImage::Format_ARGB32_Premultiplied;
    if (!rgba && !argb)
        return false;
    for (int y = 0; y < image.height(); ++y) {
        if ((y & 63) == 0 && cancelled && cancelled())
            return false;
        const uchar* pixels = image.constScanLine(y);
        if (rgba) {
            for (int x = 0; x < image.width(); ++x)
                if (pixels[x * 4 + 3] != 255)
                    return false;
        } else {
            const auto* packed = reinterpret_cast<const QRgb*>(pixels);
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(packed[x]) != 255)
                    return false;
        }
    }
    return true;
}

struct PreparedItem final {
    ScreenshotImageRowSource rows;
    QRect rect;
    bool opaque = false;
};

struct PreparedRows final {
    QVector<PreparedItem> items;
    CollageLayout layout;
    QColor background;
    std::function<bool()> cancelled;
    std::shared_ptr<collage_detail::ResourceReservation> reservation;
    std::shared_ptr<collage_detail::ResourceReservation> working;
    QVector<std::shared_ptr<void>> sourceOwners;
    std::mutex mutex;
    std::vector<uchar> scratch;

    bool cancellationRequested() const {
        return cancelled && cancelled();
    }

    bool read(int first, int count, qsizetype stride, uchar* destination, qsizetype capacity) {
        const std::lock_guard lock(mutex);
        const qsizetype rowBytes = qsizetype(layout.size.width()) * 4;
        if (cancellationRequested() || !destination || first < 0 || count <= 0 ||
            count > layout.size.height() || first > layout.size.height() - count ||
            stride < rowBytes || capacity < rowBytes ||
            qsizetype(count - 1) > (capacity - rowBytes) / stride)
            return false;

        const uchar red = static_cast<uchar>(background.red());
        const uchar green = static_cast<uchar>(background.green());
        const uchar blue = static_cast<uchar>(background.blue());
        const uchar alpha = static_cast<uchar>(background.alpha());
        for (int row = 0; row < count; ++row) {
            if (cancellationRequested())
                return false;
            uchar* pixels = destination + qsizetype(row) * stride;
            if (alpha == 0) {
                std::memset(pixels, 0, static_cast<size_t>(rowBytes));
            } else if (red == 255 && green == 255 && blue == 255 && alpha == 255) {
                std::memset(pixels, 255, static_cast<size_t>(rowBytes));
            } else {
                for (int x = 0; x < layout.size.width(); ++x) {
                    pixels[x * 4] = red;
                    pixels[x * 4 + 1] = green;
                    pixels[x * 4 + 2] = blue;
                    pixels[x * 4 + 3] = alpha;
                }
            }
        }

        for (const auto& item : items) {
            const int begin = std::max(first, item.rect.top());
            const int end = std::min(first + count, item.rect.top() + item.rect.height());
            if (begin >= end)
                continue;
            const qsizetype itemStride = qsizetype(item.rect.width()) * 4;
            const int batch = static_cast<int>(
                std::max<qsizetype>(1, std::min<qsizetype>(64, rowScratchBytes / itemStride)));
            for (int start = begin; start < end; start += batch) {
                if (cancellationRequested())
                    return false;
                const int rows = std::min(batch, end - start);
                const qsizetype bytes = itemStride * rows;
                try {
                    scratch.resize(static_cast<size_t>(bytes));
                } catch (const std::bad_alloc&) {
                    return false;
                }
                if (!item.rows.readRows(start - item.rect.top(), rows, itemStride, scratch.data(),
                                        bytes))
                    return false;
                for (int row = 0; row < rows; ++row) {
                    const uchar* source = scratch.data() + qsizetype(row) * itemStride;
                    uchar* target = destination + qsizetype(start - first + row) * stride +
                                    qsizetype(item.rect.left()) * 4;
                    if (alpha == 0 || item.opaque) {
                        std::memcpy(target, source, static_cast<size_t>(itemStride));
                        continue;
                    }
                    for (int x = 0; x < item.rect.width(); ++x) {
                        const unsigned sourceAlpha = source[3];
                        if (sourceAlpha == 255) {
                            std::memcpy(target, source, 4);
                        } else if (sourceAlpha != 0) {
                            // Keep all products until the final division, including background
                            // alpha, so translucent colors remain straight-alpha RGBA.
                            const unsigned backgroundWeight = unsigned(alpha) * (255 - sourceAlpha);
                            const unsigned coverage = sourceAlpha * 255 + backgroundWeight;
                            const unsigned values[3] = {red, green, blue};
                            for (int channel = 0; channel < 3; ++channel) {
                                const unsigned numerator =
                                    unsigned(source[channel]) * sourceAlpha * 255 +
                                    values[channel] * backgroundWeight;
                                target[channel] =
                                    static_cast<uchar>((numerator + coverage / 2) / coverage);
                            }
                            target[3] = static_cast<uchar>((coverage + 127) / 255);
                        }
                        source += 4;
                        target += 4;
                    }
                }
            }
        }
        return !cancellationRequested();
    }
};
} // namespace

QSize transformedSize(const CollageItem& item) {
    if (item.image.isNull())
        return {};
    const QRectF bounds = item.transform.mapRect(QRectF(QPointF(0, 0), item.image.size()));
    if (!std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) || bounds.width() <= 0 ||
        bounds.height() <= 0 || bounds.width() > std::numeric_limits<int>::max() ||
        bounds.height() > std::numeric_limits<int>::max())
        return {};
    return {qRound(bounds.width()), qRound(bounds.height())};
}

QImage transformedPreview(const CollageItem& item) {
    return snowCanvasTransformImage(boundedPreview(item.image, item.preview), item.transform,
                                    Qt::FastTransformation);
}

CollageLayout calculateCollageLayout(const QVector<CollageItem>& items,
                                     const CollageSettings& settings) {
    CollageLayout result;
    if (items.isEmpty())
        return result;
    if (!settingsValid(settings)) {
        result.error = QCoreApplication::translate("CollageDocument",
                                                   "The collage layout settings are invalid.");
        return result;
    }
    const bool vertical = settings.direction == CollageDirection::Vertical;
    const QSize reference = transformedSize(items.front());
    if (reference.isEmpty()) {
        result.error = preparationError();
        return result;
    }
    QVector<QSize> sizes;
    sizes.reserve(items.size());
    qint64 axis = 0;
    int cross = 0;
    for (const auto& item : items) {
        QSize size = transformedSize(item);
        if (size.isEmpty()) {
            result.error = preparationError();
            return result;
        }
        if (settings.sizing == CollageSizing::Uniform) {
            const qint64 numerator = vertical ? qint64(size.height()) * reference.width()
                                              : qint64(size.width()) * reference.height();
            const int divisor = vertical ? size.width() : size.height();
            const qint64 scaled = std::max<qint64>(1, (numerator + divisor / 2) / divisor);
            if (scaled > std::numeric_limits<int>::max()) {
                result.error = resourceError();
                return result;
            }
            size = vertical ? QSize(reference.width(), static_cast<int>(scaled))
                            : QSize(static_cast<int>(scaled), reference.height());
        }
        sizes.append(size);
        axis += vertical ? size.height() : size.width();
        if (sizes.size() > 1)
            axis += settings.spacing;
        cross = std::max(cross, vertical ? size.width() : size.height());
        if (axis > std::numeric_limits<int>::max()) {
            result.error = resourceError();
            return result;
        }
    }
    axis += qint64(settings.border) * 2;
    const qint64 fullCross = qint64(cross) + qint64(settings.border) * 2;
    if (axis > std::numeric_limits<int>::max() || fullCross > std::numeric_limits<int>::max()) {
        result.error = resourceError();
        return result;
    }
    result.size = vertical ? QSize(static_cast<int>(fullCross), static_cast<int>(axis))
                           : QSize(static_cast<int>(axis), static_cast<int>(fullCross));
    qint64 bytes = 0;
    if (!outputBytes(result.size, &bytes)) {
        result.error = resourceError();
        result.size = {};
        return result;
    }
    qint64 offset = settings.border;
    result.rects.reserve(sizes.size());
    for (const QSize& size : sizes) {
        const int centered =
            settings.border + (cross - (vertical ? size.width() : size.height())) / 2;
        result.rects.append(vertical ? QRect(QPoint(centered, static_cast<int>(offset)), size)
                                     : QRect(QPoint(static_cast<int>(offset), centered), size));
        offset += (vertical ? size.height() : size.width()) + settings.spacing;
    }
    return result;
}

struct CollageDocument::Impl final {
    QVector<CollageItem> items;
    CollageSettings settings;
    quint64 selected = 0;
    quint64 nextId = 1;
    QString error;
    qint64 sourceBytes = 0;
    qint64 previewBytes = 0;

    bool admit(qint64 sources, qint64 previews) {
        if (sources < 0 || sources > collage_detail::maximumRasterBytes || previews < 0 ||
            previews > collage_detail::maximumWindowPreviewBytes) {
            error = resourceError();
            return false;
        }
        sourceBytes = sources;
        previewBytes = previews;
        error.clear();
        return true;
    }
};

CollageDocument::CollageDocument(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {}
CollageDocument::~CollageDocument() = default;

const QVector<CollageItem>& CollageDocument::items() const {
    return m_impl->items;
}

CollageSettings CollageDocument::settings() const {
    return m_impl->settings;
}

void CollageDocument::setSettings(const CollageSettings& settings) {
    if (!settingsValid(settings) || settings == m_impl->settings)
        return;
    m_impl->settings = settings;
    emit changed();
}

CollageLayout CollageDocument::layout() const {
    return calculateCollageLayout(m_impl->items, m_impl->settings);
}

QString CollageDocument::lastError() const {
    return m_impl->error;
}

quint64 CollageDocument::append(QImage image, QString label, QImage preview) {
    if (image.isNull()) {
        m_impl->error = preparationError();
        return 0;
    }
    preview = boundedPreview(image, preview);
    if (preview.isNull()) {
        m_impl->error = preparationError();
        return 0;
    }
    try {
        m_impl->items.reserve(m_impl->items.size() + 1);
    } catch (const std::bad_alloc&) {
        m_impl->error = preparationError();
        return 0;
    }
    auto owner = sourceLifetime(image, preview, &m_impl->error);
    if (!owner)
        return 0;
    if (!m_impl->admit(m_impl->sourceBytes + image.sizeInBytes(),
                       m_impl->previewBytes + preview.sizeInBytes()))
        return 0;
    const quint64 id = m_impl->nextId++;
    m_impl->items.append(
        {id, 1, std::move(image), std::move(preview), {}, std::move(label), std::move(owner)});
    const bool selectFirst = m_impl->selected == 0;
    if (selectFirst)
        m_impl->selected = id;
    emit changed();
    if (selectFirst)
        emit selectionChanged();
    return id;
}

bool CollageDocument::replace(quint64 id, QImage image, QString label, QImage preview) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0 || image.isNull()) {
        m_impl->error = preparationError();
        return false;
    }
    preview = boundedPreview(image, preview);
    if (preview.isNull()) {
        m_impl->error = preparationError();
        return false;
    }
    auto owner = sourceLifetime(image, preview, &m_impl->error);
    if (!owner)
        return false;
    const auto& old = m_impl->items[index];
    if (!m_impl->admit(m_impl->sourceBytes - old.image.sizeInBytes() + image.sizeInBytes(),
                       m_impl->previewBytes - old.preview.sizeInBytes() + preview.sizeInBytes()))
        return false;
    auto& item = m_impl->items[index];
    item.image = std::move(image);
    item.preview = std::move(preview);
    item.label = std::move(label);
    item.resourceOwner = std::move(owner);
    item.transform = {};
    ++item.revision;
    emit changed();
    return true;
}

bool CollageDocument::remove(quint64 id) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    const auto& item = m_impl->items[index];
    static_cast<void>(m_impl->admit(m_impl->sourceBytes - item.image.sizeInBytes(),
                                    m_impl->previewBytes - item.preview.sizeInBytes()));
    m_impl->items.removeAt(index);
    const bool selectionRemoved = m_impl->selected == id;
    if (selectionRemoved)
        m_impl->selected = m_impl->items.isEmpty()
                               ? 0
                               : m_impl->items[std::min(index, int(m_impl->items.size()) - 1)].id;
    emit changed();
    if (selectionRemoved)
        emit selectionChanged();
    return true;
}

void CollageDocument::clear() {
    if (m_impl->items.isEmpty())
        return;
    m_impl->items.clear();
    m_impl->selected = 0;
    static_cast<void>(m_impl->admit(0, 0));
    emit changed();
    emit selectionChanged();
}

bool CollageDocument::swapItems(quint64 first, quint64 second) {
    const int a = itemIndex(m_impl->items, first);
    const int b = itemIndex(m_impl->items, second);
    if (a < 0 || b < 0 || a == b)
        return false;
    std::swap(m_impl->items[a], m_impl->items[b]);
    emit changed();
    return true;
}

bool CollageDocument::rotate(quint64 id, bool clockwise) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    const QTransform operation =
        clockwise ? QTransform(0, 1, -1, 0, 0, 0) : QTransform(0, -1, 1, 0, 0, 0);
    item.transform = canonicalTransform(item.transform * operation);
    ++item.revision;
    emit changed();
    return true;
}

bool CollageDocument::flip(quint64 id, bool horizontal) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    item.transform = canonicalTransform(
        item.transform * QTransform(horizontal ? -1 : 1, 0, 0, horizontal ? 1 : -1, 0, 0));
    ++item.revision;
    emit changed();
    return true;
}

bool CollageDocument::resetTransform(quint64 id) {
    const int index = itemIndex(m_impl->items, id);
    if (index < 0)
        return false;
    auto& item = m_impl->items[index];
    if (item.transform.isIdentity())
        return true;
    item.transform = {};
    ++item.revision;
    emit changed();
    return true;
}

quint64 CollageDocument::selectedId() const {
    return m_impl->selected;
}

void CollageDocument::select(quint64 id) {
    const quint64 selected = itemIndex(m_impl->items, id) >= 0 ? id : 0;
    if (selected == m_impl->selected)
        return;
    m_impl->selected = selected;
    emit selectionChanged();
}

ScreenshotImageRowSource prepareCollageRows(const QVector<CollageItem>& items,
                                            const CollageSettings& settings,
                                            std::function<bool()> cancelled, QString* error) {
    if (error)
        error->clear();
    const auto stopped = [&] { return cancelled && cancelled(); };
    if (stopped()) {
        report(error, cancellationError());
        return {};
    }
    const CollageLayout layout = calculateCollageLayout(items, settings);
    if (!layout.isValid()) {
        report(error, layout.error.isEmpty() ? preparationError() : layout.error);
        return {};
    }
    qint64 preparedBytes = 0;
    qint64 readerCacheBytes = 0;
    qint64 transformScratchBytes = 0;
    qint64 unownedSourceBytes = 0;
    qint64 maximumRowBytes = std::max(qint64(rowScratchBytes), qint64(layout.size.width()) * 4);
    for (qsizetype index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const bool transformed = !item.transform.isIdentity();
        const bool resized = transformedSize(item) != layout.rects[index].size();
        if (resized)
            preparedBytes += qint64(layout.rects[index].width()) * layout.rects[index].height() * 4;
        else if (transformed)
            preparedBytes +=
                ((qint64(layout.rects[index].width()) * item.image.depth() + 31) / 32) * 4 *
                layout.rects[index].height();
        if (!resized && (item.image.format() != QImage::Format_RGBA8888 ||
                         (item.image.colorSpace().isValid() &&
                          item.image.colorSpace() != QColorSpace(QColorSpace::SRgb))))
            readerCacheBytes +=
                std::min(conversionCacheBytes,
                         qint64(layout.rects[index].width()) * layout.rects[index].height() * 4);
        maximumRowBytes = std::max(maximumRowBytes, qint64(layout.rects[index].width()) * 4);
        if (transformed)
            transformScratchBytes =
                std::max(transformScratchBytes, qint64(item.image.sizeInBytes()));
        if (!item.resourceOwner)
            unownedSourceBytes += item.image.sizeInBytes() + item.preview.sizeInBytes();
    }
    const qint64 transientBytes =
        readerCacheBytes + transformScratchBytes + 64 * 1024 * 1024 + maximumRowBytes;
    if (transientBytes > collage_detail::maximumWorkingBytes) {
        report(error, QCoreApplication::translate(
                          "CollageDocument",
                          "This collage requires too much temporary memory. Try fewer images or "
                          "smaller image sizes."));
        return {};
    }
    if (preparedBytes > collage_detail::maximumRasterBytes) {
        report(error, resourceError());
        return {};
    }
    try {
        auto prepared = std::make_shared<PreparedRows>();
        prepared->reservation = collage_detail::reserve(preparedBytes + unownedSourceBytes, error);
        if (!prepared->reservation)
            return {};
        prepared->working = collage_detail::reserveWorking(transientBytes, error);
        if (!prepared->working)
            return {};
        prepared->layout = layout;
        prepared->background = settings.background;
        prepared->cancelled = std::move(cancelled);
        prepared->items.reserve(items.size());
        prepared->sourceOwners.reserve(items.size());
        for (qsizetype index = 0; index < items.size(); ++index) {
            if (prepared->cancellationRequested()) {
                report(error, cancellationError());
                return {};
            }
            const auto& item = items[index];
            prepared->sourceOwners.append(item.resourceOwner);
            QImage pixels =
                snowCanvasTransformImage(item.image, item.transform, Qt::FastTransformation);
            if (pixels.isNull()) {
                report(error, preparationError());
                return {};
            }
            const QSize size = layout.rects[index].size();
            if (pixels.size() != size) {
                auto input = image_codec::srgbRowSource(pixels);
                input.cancellationRequested = prepared->cancelled;
                QImage resized = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
                if (resized.isNull() ||
                    !image_codec::resizeToRgba8(input, size, resized.bits(), resized.bytesPerLine(),
                                                resized.sizeInBytes(), error)) {
                    if (!error || error->isEmpty())
                        report(error, preparationError());
                    return {};
                }
                resized.setColorSpace(QColorSpace(QColorSpace::SRgb));
                pixels = std::move(resized);
            }
            auto rows = image_codec::srgbRowSource(pixels);
            if (!rows.isValid()) {
                report(error, preparationError());
                return {};
            }
            const bool opaque = imageIsOpaque(pixels, prepared->cancelled);
            if (prepared->cancellationRequested()) {
                report(error, cancellationError());
                return {};
            }
            prepared->items.append({std::move(rows), layout.rects[index], opaque});
        }
        static_cast<void>(prepared->working->resize(readerCacheBytes + maximumRowBytes));
        ScreenshotImageRowSource rows;
        rows.size = layout.size;
        rows.cancellationRequested = prepared->cancelled;
        rows.readRows = [prepared](int first, int count, qsizetype stride, uchar* destination,
                                   qsizetype capacity) {
            return prepared->read(first, count, stride, destination, capacity);
        };
        return rows;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}

QImage materializeCollageRows(const ScreenshotImageRowSource& rows, qsizetype maximumBytes) {
    qint64 bytes = 0;
    if (!rows.isValid() || !outputBytes(rows.size, &bytes) || bytes > maximumBytes ||
        (rows.cancellationRequested && rows.cancellationRequested()))
        return {};
    QImage image = snowCanvasAllocateImage(rows.size, QImage::Format_RGBA8888);
    if (image.isNull() ||
        !rows.readRows(0, image.height(), image.bytesPerLine(), image.bits(), image.sizeInBytes()))
        return {};
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

CollageClipboardEstimate estimateCollageClipboard(QSize outputSize) {
    CollageClipboardEstimate result;
    qint64 rawBytes = 0;
    if (!outputBytes(outputSize, &rawBytes)) {
        result.error = resourceError();
        return result;
    }
    constexpr qint64 encodingOverhead = 64 * 1024 * 1024;
#if defined(Q_OS_WIN)
    const qint64 nativeStride = (qint64(outputSize.width()) * 3 + 3) & ~qint64{3};
    result.dibBytes = nativeStride * outputSize.height();
    result.rowScratchBytes = qint64(outputSize.width()) * 4 * std::min(64, outputSize.height());
    result.bytes = rawBytes * 2 + result.dibBytes + result.rowScratchBytes + encodingOverhead;
#else
    result.bytes = rawBytes + encodingOverhead;
#endif
    if (result.bytes > qint64{16} * 1024 * 1024 * 1024 ||
        result.dibBytes > std::numeric_limits<quint32>::max())
        result.error = QCoreApplication::translate(
            "CollageDocument",
            "The collage is too large for the clipboard. Save it to a file instead.");
    return result;
}

std::shared_ptr<void> reserveCollageClipboard(QSize outputSize, QString* error) {
    const auto estimate = estimateCollageClipboard(outputSize);
    if (!estimate.isValid()) {
        report(error, estimate.error);
        return {};
    }
    return collage_detail::reserve(estimate.bytes, error);
}

CollageResourceUsage collageResourceUsage() {
    return {collage_detail::reservedBytes(), collage_detail::budgetLimit()};
}

qint64 collageImageImportBytes(QSize size, QString* error) {
    if (error)
        error->clear();
    if (size.isEmpty()) {
        report(error, preparationError());
        return 0;
    }
    const qint64 pixels = qint64(size.width()) * size.height();
    if (pixels > 64'000'000) {
        report(error, QCoreApplication::translate(
                          "CollageDocument",
                          "This collage requires too much temporary memory. Try fewer images "
                          "or smaller image sizes."));
        return 0;
    }
    return pixels * 8;
}

QImage captureCollageImage(QSize size, const std::function<QImage()>& capture, QString* error) {
    const qint64 bytes = collageImageImportBytes(size, error) / 2;
    if (bytes == 0)
        return {};
    if (!capture) {
        report(error, preparationError());
        return {};
    }
    try {
        auto working = collage_detail::reserveWorking(bytes, error);
        if (!working)
            return {};
        auto lifetime = std::make_unique<CapturedImageLifetime>();
        lifetime->reservation = collage_detail::reserve(bytes, error);
        if (!lifetime->reservation)
            return {};
        lifetime->image = capture();
        const QImage image = lifetime->image;
        if (image.isNull() || image.size() != size || image.sizeInBytes() > bytes) {
            report(error, preparationError());
            return {};
        }
        static_cast<void>(lifetime->reservation->resize(image.sizeInBytes()));
        // A const view keeps admission attached to QImage's implicitly shared pixel lifetime.
        // Metadata changes below do not duplicate the raster; writers detach as usual.
        QImage result(
            image.constBits(), image.width(), image.height(), image.bytesPerLine(), image.format(),
            [](void* context) { delete static_cast<CapturedImageLifetime*>(context); },
            lifetime.get());
        if (result.isNull()) {
            report(error, preparationError());
            return {};
        }
        [[maybe_unused]] const auto* imageOwner = lifetime.release();
        result.setColorSpace(image.colorSpace());
        result.setDevicePixelRatio(image.devicePixelRatio());
        result.setDotsPerMeterX(image.dotsPerMeterX());
        result.setDotsPerMeterY(image.dotsPerMeterY());
        result.setOffset(image.offset());
        result.setColorTable(image.colorTable());
        for (const auto& key : image.textKeys())
            result.setText(key, image.text(key));
        return result;
    } catch (const std::bad_alloc&) {
        report(error, preparationError());
        return {};
    }
}
} // namespace snow_shot::presentation

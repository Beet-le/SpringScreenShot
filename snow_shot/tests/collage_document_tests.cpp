// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"

#include <QCoreApplication>
#include <QColorSpace>
#include <QPainter>

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace snow_shot::presentation;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage fixture(QSize size, QColor color = Qt::red) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

QImage asymmetricFixture() {
    QImage image = fixture({3, 2});
    const std::array<QColor, 6> colors = {Qt::red,    Qt::green,   Qt::blue,
                                          Qt::yellow, Qt::magenta, Qt::cyan};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            image.setPixelColor(x, y, colors[size_t(y) * 3 + size_t(x)]);
    return image;
}

void geometry() {
    CollageDocument document;
    require(!document.layout().isValid() && document.selectedId() == 0,
            "empty document has no layout or selection");
    require(document.settings() == CollageSettings{}, "initial settings match specification");
    const auto a = document.append(fixture({4, 2}), QStringLiteral("a"));
    document.append(fixture({2, 4}), QStringLiteral("b"));
    document.append(fixture({3, 3}), QStringLiteral("c"));
    require(document.selectedId() == a, "first import selects first image");
    auto layout = document.layout();
    require(layout.size == QSize(4, 14) && layout.rects[1] == QRect(0, 2, 4, 8),
            "uniform vertical geometry references first width");

    CollageSettings settings;
    settings.sizing = CollageSizing::Original;
    settings.border = 2;
    settings.spacing = 3;
    document.setSettings(settings);
    layout = document.layout();
    require(layout.size == QSize(8, 19), "border bands and adjacent spacing appear once");
    require(layout.rects ==
                QVector<QRect>{QRect(2, 2, 4, 2), QRect(3, 7, 2, 4), QRect(2, 14, 3, 3)},
            "original vertical images center with deterministic odd-pixel placement");

    settings.direction = CollageDirection::Horizontal;
    document.setSettings(settings);
    layout = document.layout();
    require(layout.size == QSize(19, 8) && layout.rects[0] == QRect(2, 3, 4, 2) &&
                layout.rects[1] == QRect(9, 2, 2, 4),
            "original horizontal images center vertically");

    settings.sizing = CollageSizing::Uniform;
    document.setSettings(settings);
    require(document.layout().size == QSize(17, 6) &&
                document.layout().rects[1].size() == QSize(1, 2),
            "uniform horizontal images reference first height");

    settings.border = std::numeric_limits<int>::max();
    document.setSettings(settings);
    require(!document.layout().isValid() && !document.layout().error.isEmpty(),
            "integer overflow and output limit produce an actionable layout error");
    settings.border = -1;
    document.setSettings(settings);
    require(document.settings().border == std::numeric_limits<int>::max(),
            "negative settings do not replace valid settings");

    CollageDocument single;
    QImage dpiImage = fixture({4, 2});
    dpiImage.setDevicePixelRatio(2);
    single.append(dpiImage, QStringLiteral("dpi"));
    CollageSettings singleSettings;
    singleSettings.spacing = std::numeric_limits<int>::max();
    single.setSettings(singleSettings);
    require(single.layout().size == QSize(4, 2) &&
                materializeCollageRows(prepareCollageRows(single.items(), singleSettings)).size() ==
                    QSize(4, 2),
            "output pixels are DPI independent and single-image spacing cannot overflow");
}

void referenceAndManagement() {
    CollageDocument document;
    const auto a = document.append(fixture({4, 2}), QStringLiteral("a"));
    const auto b = document.append(fixture({2, 4}), QStringLiteral("b"));
    require(document.layout().size == QSize(4, 10), "initial reference is first item");
    require(document.rotate(a, true) && document.layout().size == QSize(2, 8),
            "rotating first item changes uniform reference immediately");
    require(document.swapItems(a, b) && document.items()[0].id == b &&
                document.items()[1].id == a && document.selectedId() == a,
            "swap preserves identity and selection");
    require(!document.swapItems(a, 9999) && !document.swapItems(a, a),
            "invalid and self swaps do not change ordering");
    document.resetTransform(a);
    require(document.layout().size == QSize(2, 5), "reset applies only to selected item");
    document.rotate(b, true);
    require(document.replace(b, fixture({6, 3}, Qt::blue), QStringLiteral("replacement")),
            "replacement succeeds");
    require(document.items()[0].id == b && document.items()[0].transform.isIdentity() &&
                document.layout().size == QSize(6, 6),
            "replacement preserves position, resets transform, and changes first reference");
    const auto oldKey = document.items()[0].image.cacheKey();
    require(!document.replace(b, {}, QStringLiteral("failed")) &&
                document.items()[0].image.cacheKey() == oldKey,
            "failed replacement keeps original item intact");
    require(document.remove(b) && document.layout().size == QSize(4, 2),
            "deleting first item changes reference");
    require(document.remove(a) && !document.layout().isValid() && document.selectedId() == 0,
            "deleting final image produces empty state");
    require(document.append({}, QStringLiteral("failed")) == 0 && !document.lastError().isEmpty(),
            "failed import is reported without adding an item");
    const auto duplicate = fixture({2, 2});
    const auto first = document.append(duplicate, QStringLiteral("duplicate"));
    const auto second = document.append(duplicate, QStringLiteral("duplicate"));
    require(first != second && document.items().size() == 2,
            "repeated source images remain independent collection items");
    document.clear();
    require(document.items().isEmpty() && document.selectedId() == 0,
            "clear resets collection and selection");
}

void transformations() {
    CollageDocument document;
    const QImage source = asymmetricFixture();
    const auto id = document.append(source, QStringLiteral("source"));
    const auto sourceKey = source.cacheKey();
    auto output = [&] {
        CollageSettings settings;
        settings.sizing = CollageSizing::Original;
        return materializeCollageRows(prepareCollageRows(document.items(), settings));
    };
    document.rotate(id, true);
    QImage rotated = output();
    require(rotated.size() == QSize(2, 3) && rotated.pixelColor(1, 0) == Qt::red &&
                rotated.pixelColor(0, 0) == Qt::yellow && rotated.pixelColor(1, 2) == Qt::blue,
            "clockwise quarter-turn is exact");
    document.flip(id, true);
    QImage flipped = output();
    require(flipped.pixelColor(0, 0) == Qt::red && flipped.pixelColor(1, 0) == Qt::yellow,
            "horizontal flip uses current visual orientation");
    document.flip(id, false);
    flipped = output();
    require(flipped.pixelColor(0, 2) == Qt::red && flipped.pixelColor(1, 2) == Qt::yellow,
            "vertical flip uses current visual orientation");
    document.resetTransform(id);
    require(output() == source, "reset restores source pixel order");
    document.rotate(id, false);
    rotated = output();
    require(rotated.pixelColor(0, 0) == Qt::blue && rotated.pixelColor(1, 0) == Qt::cyan,
            "counterclockwise quarter-turn is exact");
    document.rotate(id, true);
    require(output() == source && document.items()[0].image.cacheKey() == sourceKey,
            "inverse rotations restore transform without altering original pixels");
    require(transformedPreview(document.items()[0]).size() == source.size(),
            "small previews reflect current transform");
    CollageItem large;
    large.image = fixture({400, 300});
    large.transform = QTransform(0, 1, -1, 0, 0, 0);
    require(transformedPreview(large).size() == QSize(192, 256),
            "missing previews derive bounded images before transforming");
}

void alphaAndRowReads() {
    CollageDocument document;
    QImage image = fixture({2, 2}, QColor(255, 0, 0, 128));
    image.setPixelColor(1, 1, Qt::transparent);
    document.append(image, QStringLiteral("alpha"));
    CollageSettings settings;
    settings.border = 1;
    settings.background = QColor(0, 0, 255, 128);
    document.setSettings(settings);
    QString error;
    const auto rows = prepareCollageRows(document.items(), settings, {}, &error);
    require(rows.isValid() && rows.backingImage.isNull() && error.isEmpty(),
            "export returns an immutable row source without materializing full output");
    const QImage output = materializeCollageRows(rows);
    require(output.size() == QSize(4, 4) && output.pixelColor(0, 0) == settings.background,
            "outer band retains semitransparent background");
    const QColor color = output.pixelColor(1, 1);
    require(color.red() == 170 && color.blue() == 85 && color.alpha() == 192,
            "straight alpha composition retains semitransparency");
    require(output.pixelColor(2, 2) == settings.background,
            "fully transparent source reveals background");

    const qsizetype stride = output.width() * 4 + 7;
    QByteArray buffer(stride * output.height(), '\x5a');
    require(rows.readRows(1, 2, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()),
            "arbitrary multi-row reads accept padded strides");
    require(std::memcmp(buffer.constData(), output.constScanLine(1), size_t(output.width()) * 4) ==
                    0 &&
                std::memcmp(buffer.constData() + stride, output.constScanLine(2),
                            size_t(output.width()) * 4) == 0 &&
                buffer[output.width() * 4] == '\x5a',
            "partial reads match whole output and leave padding untouched");
    require(
        !rows.readRows(-1, 1, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()) &&
            !rows.readRows(3, 2, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()) &&
            !rows.readRows(0, 2, stride, reinterpret_cast<uchar*>(buffer.data()), 1),
        "invalid destination ranges are rejected safely");
    require(materializeCollageRows(rows, 1).isNull(),
            "test materializer obeys explicit allocation limit");

    settings.background = Qt::transparent;
    const QImage transparent =
        materializeCollageRows(prepareCollageRows(document.items(), settings));
    require(transparent.pixelColor(1, 1) == image.pixelColor(0, 0) &&
                transparent.pixelColor(0, 0).alpha() == 0,
            "transparent backgrounds preserve source alpha");
}

void snapshotsAndResources() {
    const qint64 baseline = collageResourceUsage().reservedBytes;
    QVector<CollageItem> snapshot;
    {
        CollageDocument first;
        CollageDocument second;
        first.append(fixture({20, 10}), QStringLiteral("first"));
        second.append(fixture({10, 20}), QStringLiteral("second"));
        auto settings = first.settings();
        settings.direction = CollageDirection::Horizontal;
        first.setSettings(settings);
        first.rotate(first.items()[0].id, true);
        require(second.settings().direction == CollageDirection::Vertical &&
                    second.items()[0].transform.isIdentity(),
                "document editing and settings remain independent");
        snapshot = first.items();
    }
    require(collageResourceUsage().reservedBytes > baseline,
            "snapshots retain source memory admission after window destruction");
    auto rows = prepareCollageRows(snapshot, CollageSettings{});
    snapshot.clear();
    require(rows.isValid() && collageResourceUsage().reservedBytes > baseline &&
                !materializeCollageRows(rows).isNull(),
            "prepared rows keep snapshot source storage and reservations alive");
    rows = {};
    require(collageResourceUsage().reservedBytes == baseline,
            "last reader retirement releases all resource reservations");

    QString error;
    auto reservation = reserveCollageImport(1, &error);
    require(reservation && reservation->resize(2, &error), "import reservation can grow");
    require(!reservation->resize(qint64{513} * 1024 * 1024, &error) && !error.isEmpty(),
            "transient import work has deterministic shared resource limits");
    reservation.reset();
    require(collageResourceUsage().reservedBytes == baseline,
            "rejected reservation growth retains and later releases prior admission");

    CollageDocument document;
    document.append(fixture({2, 2}), QStringLiteral("cancel"));
    std::atomic_bool cancel{true};
    require(!prepareCollageRows(
                 document.items(), document.settings(), [&] { return cancel.load(); }, &error)
                    .isValid() &&
                !error.isEmpty(),
            "already cancelled preparation returns clear failure");
    cancel.store(false);
    auto cancellable = prepareCollageRows(
        document.items(), document.settings(), [&] { return cancel.load(); }, &error);
    cancel.store(true);
    require(materializeCollageRows(cancellable).isNull(), "row production honors cancellation");
    require(!reserveCollageClipboard({2000000, 1000000}, &error) && !error.isEmpty(),
            "clipboard resource limits fail before native allocations");

    const auto small = estimateCollageClipboard({1, 2});
    require(small.isValid(),
            "small clipboard estimates are independent of current available memory");
#ifdef Q_OS_WIN
    require(small.dibBytes == 8 && small.rowScratchBytes == 8 &&
                small.bytes == qint64{64} * 1024 * 1024 + 32,
            "clipboard estimate includes padded DIB stride, both PNG representations, and scratch");
    const auto wide = estimateCollageClipboard({384000, 2160});
    require(wide.isValid() && wide.rowScratchBytes == qint64{384000} * 4 * 64 &&
                wide.dibBytes == qint64{384000} * 3 * 2160 &&
                wide.bytes ==
                    qint64{384000} * 2160 * 11 + wide.rowScratchBytes + qint64{64} * 1024 * 1024,
            "horizontal stress admission includes the complete 64-row native scratch buffer");
    require(!estimateCollageClipboard({449350, 3840}).isValid(),
            "mixed horizontal stress output is rejected before overflowing native DIB capacity");
#else
    require(small.dibBytes == 0 && small.rowScratchBytes == 0 &&
                small.bytes == qint64{64} * 1024 * 1024 + 8,
            "non-Windows clipboard estimates cover PNG preparation without a native DIB");
#endif
    require(!estimateCollageClipboard({}).isValid() &&
                !estimateCollageClipboard({-1, 2}).isValid() &&
                !estimateCollageClipboard({std::numeric_limits<int>::max(), 1}).isValid(),
            "clipboard dimension and stride bounds reject invalid sizes without allocating");
}

void capturedImagesAdmitMemoryBeforePaintingAndOwnTheirReservation() {
    const qint64 baseline = collageResourceUsage().reservedBytes;
    int captures = 0;
    const auto capture = [&] {
        ++captures;
        return fixture({3, 2});
    };
    QString error;
    require(captureCollageImage({8001, 8000}, capture, &error).isNull() && captures == 0 &&
                !error.isEmpty(),
            "oversized captures fail before calling the rasterizer");
    auto occupied = reserveCollageImport(qint64{512} * 1024 * 1024);
    require(occupied != nullptr, "occupy the shared painting budget without allocating pixels");
    require(captureCollageImage({3, 2}, capture, &error).isNull() && captures == 0 &&
                !error.isEmpty(),
            "memory-limited captures fail before calling the rasterizer");
    occupied.reset();
    require(collageResourceUsage().reservedBytes == baseline,
            "rejected captures leave no reservations behind");

    const uchar* capturedPixels = nullptr;
    QImage captured = captureCollageImage(
        {3, 2},
        [&] {
            QImage pixels = fixture({3, 2}, QColor(50, 100, 150, 96));
            pixels.setDevicePixelRatio(2);
            pixels.setDotsPerMeterX(1234);
            pixels.setDotsPerMeterY(5678);
            pixels.setOffset({7, 9});
            pixels.setText(QStringLiteral("description"), QStringLiteral("captured image"));
            capturedPixels = pixels.constBits();
            return pixels;
        },
        &error);
    require(!captured.isNull() && error.isEmpty() && captured.constBits() == capturedPixels &&
                captured.colorSpace() == QColorSpace(QColorSpace::SRgb) &&
                captured.devicePixelRatio() == 2 && captured.dotsPerMeterX() == 1234 &&
                captured.dotsPerMeterY() == 5678 && captured.offset() == QPoint(7, 9) &&
                captured.text(QStringLiteral("description")) == QStringLiteral("captured image") &&
                captured.pixelColor(1, 1) == QColor(50, 100, 150, 96) &&
                collageResourceUsage().reservedBytes == baseline + captured.sizeInBytes(),
            "captured pixels own their admission without copying or losing image metadata");
    QImage queuedCopy = captured;
    captured = {};
    require(collageResourceUsage().reservedBytes == baseline + queuedCopy.sizeInBytes(),
            "a queued image copy keeps its raster accounted for");
    occupied = reserveCollageImport(qint64{512} * 1024 * 1024);
    require(occupied != nullptr, "retained captures do not occupy the temporary painting budget");
    occupied.reset();
    queuedCopy = {};
    require(collageResourceUsage().reservedBytes == baseline,
            "the last image reader releases the captured raster reservation");
    require(captureCollageImage(
                {3, 2}, [] { return QImage{}; }, &error)
                    .isNull() &&
                !error.isEmpty() && collageResourceUsage().reservedBytes == baseline,
            "failed rasterization releases both retained and working reservations");
    require(collageImageImportBytes({8000, 8000}, &error) == 512'000'000 && error.isEmpty() &&
                collageImageImportBytes({}, &error) == 0 && !error.isEmpty(),
            "capture and import share inclusive pixel limits and reject invalid dimensions");
}

void scalingAndLargeAxes() {
    CollageDocument document;
    document.append(fixture({2, 2}, Qt::blue), QStringLiteral("reference"));
    document.append(fixture({4, 2}, QColor(255, 0, 0, 128)), QStringLiteral("scaled"));
    CollageSettings settings;
    settings.background = Qt::transparent;
    const auto rows = prepareCollageRows(document.items(), settings);
    const QImage output = materializeCollageRows(rows);
    require(output.size() == QSize(2, 3) && output.pixelColor(0, 2).red() >= 254 &&
                output.pixelColor(0, 2).alpha() == 128,
            "uniform resizing preserves contents and alpha without cropping");

    QVector<CollageItem> items;
    for (int index = 0; index < 40; ++index)
        items.append({quint64(index + 1),
                      1,
                      fixture({1000, 1}, Qt::green),
                      {},
                      {},
                      QString::number(index),
                      {}});
    settings.direction = CollageDirection::Horizontal;
    settings.sizing = CollageSizing::Original;
    const auto wide = prepareCollageRows(items, settings);
    const QImage result = materializeCollageRows(wide);
    require(result.size() == QSize(40000, 1) && result.pixelColor(39999, 0) == Qt::green,
            "row composition remains correct beyond raster painter coordinate limits");

    QVector<CollageItem> thinItems;
    thinItems.reserve(120000);
    const QImage thinImage = fixture({1000, 1});
    for (int index = 0; index < 120000; ++index)
        thinItems.append({quint64(index + 1), 1, thinImage, {}, {}, {}, {}});
    const auto thinLayout = calculateCollageLayout(thinItems, settings);
    require(thinLayout.isValid() && thinLayout.size == QSize(120000000, 1),
            "very wide single-row geometry fits the output image limit");
    QString error;
    int cancellationChecks = 0;
    const qint64 baseline = collageResourceUsage().reservedBytes;
    const auto rejected = prepareCollageRows(
        thinItems, settings,
        [&] {
            ++cancellationChecks;
            return false;
        },
        &error);
    require(!rejected.isValid() && !error.isEmpty() && cancellationChecks == 1 &&
                collageResourceUsage().reservedBytes == baseline,
            "complete output row scratch is rejected before preparing items or reading pixels");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        geometry();
        referenceAndManagement();
        transformations();
        alphaAndRowReads();
        snapshotsAndResources();
        capturedImagesAdmitMemoryBeforePaintingAndOwnTheirReservation();
        scalingAndLargeAxes();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Collage document tests passed\n";
    return 0;
}

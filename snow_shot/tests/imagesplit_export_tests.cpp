// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snowimageqtcodec.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTranslator>

#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage fixture() {
    QImage image(7, 5, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.setDevicePixelRatio(2);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(17 + 23 * x, 31 + 29 * y, 41 + x + y, (x + y) * 23));
    return image;
}

void verifyOutputs(const QImage& image, const ImageSplitLayout& layout,
                   const ImageSplitExportResult& result) {
    require(result.succeeded() && result.savedCount == layout.outputCount(),
            "all split outputs are saved successfully");
    QImage rebuilt(image.size(), QImage::Format_RGBA8888);
    rebuilt.fill(Qt::transparent);
    const QDir directory(result.directory);
    const auto files = directory.entryList(QDir::Files);
    require(files.size() == layout.outputCount(), "output contains exactly the numbered PNG files");
    for (qint64 index = 0; index < layout.outputCount(); ++index) {
        const QString name = QStringLiteral("%1.png").arg(index + 1);
        const QString path = directory.filePath(name);
        require(files.contains(name), "PNG names are consecutive one-based export ordinals");
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) &&
                    file.read(8) == QByteArray::fromHex("89504e470d0a1a0a"),
                "each output is a PNG file");
        const QImage decoded = snow_shot::image_codec::decodeFile(path, snow::image::Format::png)
                                   .convertToFormat(QImage::Format_RGBA8888);
        const QRect region = layout.regionAt(index);
        require(!decoded.isNull() && decoded.size() == region.size(),
                "output dimensions match full-resolution region pixels");
        for (int y = 0; y < decoded.height(); ++y) {
            std::memcpy(rebuilt.scanLine(region.y() + y) + qsizetype(region.x()) * 4,
                        decoded.constScanLine(y), size_t(decoded.width()) * 4);
            for (int x = 0; x < decoded.width(); ++x)
                require(decoded.pixelColor(x, y) ==
                            image.pixelColor(region.x() + x, region.y() + y),
                        "export order, colors and transparency match source pixels");
        }
    }
    for (int y = 0; y < rebuilt.height(); ++y)
        require(std::memcmp(rebuilt.constScanLine(y), image.constScanLine(y),
                            size_t(image.width()) * 4) == 0,
                "reassembled regions preserve every straight-alpha pixel exactly");
}

void orderingAndUniqueDirectories() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "export temporary directory exists");
    const QImage image = fixture();
    const QByteArray original(reinterpret_cast<const char*>(image.constBits()),
                              image.sizeInBytes());
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 3;
    const auto layout = calculateImageSplitLayout(image.size(), settings);
    std::vector<qint64> progress;
    const auto first = exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"),
                                         {}, [&progress, &layout](qint64 count, qint64 total) {
                                             require(total == layout.outputCount(),
                                                     "progress includes actual output count");
                                             progress.push_back(count);
                                         });
    require(QFileInfo(first.directory).fileName() == QStringLiteral("Split Images_sample"),
            "base output directory follows the requested filename pattern");
    require(progress == std::vector<qint64>{0, 1, 2, 3, 4, 5, 6},
            "progress begins before writing and advances once per saved image");
    verifyOutputs(image, layout, first);
    const auto second =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"));
    const auto third = exportSplitImages(image, layout, temporary.path(), QStringLiteral("sample"));
    require(QFileInfo(second.directory).fileName() == QStringLiteral("Split Images_sample (2)") &&
                QFileInfo(third.directory).fileName() == QStringLiteral("Split Images_sample (3)"),
            "existing output folders produce unique numbered directories");
    verifyOutputs(image, layout, second);
    verifyOutputs(image, layout, third);
    const QString longStem(200, u's');
    const auto longName = exportSplitImages(image, layout, temporary.path(), longStem);
    require(longName.succeeded() && QFileInfo(longName.directory).fileName() ==
                                        QStringLiteral("Split Images_%1").arg(longStem),
            "valid long filename stems retain their complete name without arbitrary truncation");
    verifyOutputs(image, layout, longName);
    require(QByteArray(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()) ==
                original,
            "batch export never changes source pixels");
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = 3;
    const auto vertical = calculateImageSplitLayout(image.size(), settings);
    verifyOutputs(image, vertical,
                  exportSplitImages(image, vertical, temporary.path(), QStringLiteral("vertical")));
    settings.direction = ImageSplitDirection::Horizontal;
    const auto horizontal = calculateImageSplitLayout(image.size(), settings);
    verifyOutputs(
        image, horizontal,
        exportSplitImages(image, horizontal, temporary.path(), QStringLiteral("horizontal")));
}

void failureAndCancellation() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failure temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto initialEntries =
        QDir(temporary.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    const auto canceledBefore = exportSplitImages(image, layout, temporary.path(),
                                                  QStringLiteral("canceled"), [] { return true; });
    require(!canceledBefore.succeeded() && canceledBefore.directory.isEmpty() &&
                canceledBefore.savedCount == 0 && !canceledBefore.error.isEmpty() &&
                QDir(temporary.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries) ==
                    initialEntries,
            "cancellation before export creates no filesystem content");
    auto invalid = layout;
    invalid.verticalLines = {0};
    const auto invalidExport =
        exportSplitImages(image, invalid, temporary.path(), QStringLiteral("invalid"));
    require(!invalidExport.succeeded() && invalidExport.directory.isEmpty(),
            "invalid geometry is rejected before directory creation");
    const auto missingParent = exportSplitImages(
        image, layout, temporary.filePath(QStringLiteral("absent")), QStringLiteral("missing"));
    require(!missingParent.succeeded() && !missingParent.error.isEmpty() &&
                missingParent.directory.isEmpty(),
            "unavailable destination reports failure without creating parent paths");
    bool canceled = false;
    const auto partial = exportSplitImages(
        image, layout, temporary.path(), QStringLiteral("partial"),
        [&canceled] { return canceled; },
        [&canceled](qint64 saved, qint64) { canceled = saved == 1; });
    require(!partial.succeeded() && partial.savedCount == 1 && !partial.error.isEmpty() &&
                QFileInfo::exists(QDir(partial.directory).filePath(QStringLiteral("1.png"))) &&
                !QFileInfo::exists(QDir(partial.directory).filePath(QStringLiteral("2.png"))),
            "cancellation retains completed outputs and does not publish a partial next PNG");
    const QString failureDirectory = temporary.filePath(QStringLiteral("Split Images_failure"));
    const auto failed =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("failure"), {},
                          [&failureDirectory](qint64 saved, qint64) {
                              if (saved == 1)
                                  require(QDir(failureDirectory).mkdir(QStringLiteral("2.png")),
                                          "failure fixture obstructs the second PNG");
                          });
    require(!failed.succeeded() && failed.savedCount == 1 && !failed.error.isEmpty() &&
                QFileInfo::exists(QDir(failed.directory).filePath(QStringLiteral("1.png"))) &&
                QFileInfo(QDir(failed.directory).filePath(QStringLiteral("2.png"))).isDir(),
            "write failure preserves completed files and identifies failed output");
    QFile collision(temporary.filePath(QStringLiteral("Split Images_collision")));
    require(collision.open(QIODevice::WriteOnly) && collision.write("keep") == 4,
            "file collision fixture exists");
    collision.close();
    const auto fileCollision =
        exportSplitImages(image, layout, temporary.path(), QStringLiteral("collision"));
    require(fileCollision.succeeded() &&
                QFileInfo(fileCollision.directory).fileName() ==
                    QStringLiteral("Split Images_collision (2)") &&
                collision.open(QIODevice::ReadOnly) && collision.readAll() == QByteArray("keep"),
            "an existing file also forces a unique directory without overwriting it");
}

void concurrentDirectoryCreationAndNormalization() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "concurrent temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto exportWork = [&] {
        return exportSplitImages(image, layout, temporary.path(), QStringLiteral("concurrent"));
    };
    auto first = std::async(std::launch::async, exportWork);
    auto second = std::async(std::launch::async, exportWork);
    const auto firstResult = first.get();
    const auto secondResult = second.get();
    require(firstResult.succeeded() && secondResult.succeeded() &&
                firstResult.directory != secondResult.directory,
            "simultaneous exports reserve different directories atomically");
    verifyOutputs(image, layout, firstResult);
    verifyOutputs(image, layout, secondResult);
    const QImage premultiplied = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const auto normalized = premultiplied.convertToFormat(QImage::Format_RGBA8888);
    verifyOutputs(
        normalized, layout,
        exportSplitImages(premultiplied, layout, temporary.path(), QStringLiteral("normalized")));
}
void failuresRetranslate() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "language-switch temporary directory exists");
    const QImage image = fixture();
    const auto layout = calculateImageSplitLayout(image.size(), {});
    const auto canceled = exportSplitImages(image, layout, temporary.path(),
                                            QStringLiteral("language"), [] { return true; });
    require(canceled.errorSource == QStringLiteral("Export was canceled.") &&
                canceled.errorArguments.isEmpty() && canceled.errorMessage() == canceled.error,
            "helper failures retain their original translation key and initial display message");
    ImageSplitExportResult indexed;
    indexed.errorSource = QStringLiteral("Image %1 could not be saved: %2");
    indexed.errorArguments = {QStringLiteral("7"), QStringLiteral("native detail")};
    indexed.error = indexed.errorMessage();
    for (const auto* catalog : {":/i18n/snow_shot_zh_CN.qm", ":/i18n/snow_shot_zh_TW.qm"}) {
        QTranslator translator;
        require(translator.load(QString::fromLatin1(catalog)),
                "export failures require complete Chinese translation catalogs");
        QCoreApplication::installTranslator(&translator);
        const QString expected =
            QCoreApplication::translate("ImageSplitExport", "Export was canceled.");
        require(canceled.errorMessage() == expected && canceled.errorMessage() != canceled.error,
                "cached export failures retranslate when the runtime language changes");
        require(
            indexed.errorMessage() ==
                QCoreApplication::translate("ImageSplitExport", "Image %1 could not be saved: %2")
                    .arg(QStringLiteral("7"), QStringLiteral("native detail")),
            "runtime retranslation preserves ordinal and native error-detail arguments");
        QCoreApplication::removeTranslator(&translator);
    }
    require(canceled.errorMessage() == canceled.error && indexed.errorMessage() == indexed.error,
            "returning to English restores the initial error text");
    const ImageSplitExportResult external{{}, QStringLiteral("external failure"), 0, 1};
    require(external.errorMessage() == external.error,
            "existing four-field aggregate service results retain their error text");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        orderingAndUniqueDirectories();
        failureAndCancellation();
        concurrentDirectoryCreationAndNormalization();
        failuresRetranslate();
        std::cout << "Image split export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

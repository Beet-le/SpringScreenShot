// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace snow_shot::presentation {
namespace {
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageSplitExport", "Export was canceled."),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The image or split configuration is invalid."),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The image could not be prepared for export."),
    QT_TRANSLATE_NOOP("ImageSplitExport", "The output folder could not be created."),
    QT_TRANSLATE_NOOP("ImageSplitExport", "Image %1 could not be saved: %2"),
    QT_TRANSLATE_NOOP("ImageSplitExport", "There is not enough memory to export the image."),
};
void setError(ImageSplitExportResult& result, const char* source, QStringList arguments = {}) {
    result.errorSource = QString::fromUtf8(source);
    result.errorArguments = std::move(arguments);
    result.error = result.errorMessage();
}

QString safeImageName(QString name) {
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")),
                 QStringLiteral("_"));
    name = name.trimmed();
    while (name.endsWith(u'.') || name.endsWith(u' '))
        name.chop(1);
    return name.isEmpty() ? QStringLiteral("Screenshot") : name;
}

QString createExportDirectory(const QString& parentDirectory, const QString& imageName) {
    const QFileInfo parentInfo(parentDirectory);
    if (!parentInfo.isDir())
        return {};
    QDir parent(parentInfo.absoluteFilePath());
    const QString base = QStringLiteral("Split Images_%1").arg(safeImageName(imageName));
    for (qint64 suffix = 1; suffix <= std::numeric_limits<int>::max(); ++suffix) {
        const QString name = suffix == 1 ? base : QStringLiteral("%1 (%2)").arg(base).arg(suffix);
        if (parent.mkdir(name))
            return parent.absoluteFilePath(name);
        if (!QFileInfo::exists(parent.filePath(name)))
            return {};
    }
    return {};
}

ScreenshotImageRowSource cropRows(QImage image, QRect region,
                                  std::function<bool()> cancellationRequested) {
    ScreenshotImageRowSource rows;
    rows.size = region.size();
    rows.cancellationRequested = cancellationRequested;
    rows.readRows = [image = std::move(image), region,
                     cancelled = std::move(cancellationRequested)](
                        int firstRow, int rowCount, qsizetype stride, uchar* destination,
                        qsizetype destinationSize) {
        if ((cancelled && cancelled()) || firstRow < 0 || rowCount < 0 ||
            firstRow > region.height() || rowCount > region.height() - firstRow)
            return false;
        const qsizetype rowBytes = qsizetype(region.width()) * 4;
        if (rowCount == 0)
            return true;
        if (!destination || stride < rowBytes || destinationSize < rowBytes ||
            qsizetype(rowCount - 1) > (destinationSize - rowBytes) / stride)
            return false;
        for (int row = 0; row < rowCount; ++row) {
            if (cancelled && cancelled())
                return false;
            const auto* source =
                image.constScanLine(region.y() + firstRow + row) + qsizetype(region.x()) * 4;
            std::memcpy(destination + qsizetype(row) * stride, source, size_t(rowBytes));
        }
        return true;
    };
    return rows;
}
} // namespace

bool ImageSplitExportResult::succeeded() const {
    return !directory.isEmpty() && error.isEmpty() && totalCount > 0 && savedCount == totalCount;
}

QString ImageSplitExportResult::errorMessage() const {
    if (errorSource.isEmpty())
        return error;
    const auto source = errorSource.toUtf8();
    QString message = QCoreApplication::translate("ImageSplitExport", source.constData());
    for (const auto& argument : errorArguments)
        message = message.arg(argument);
    return message;
}

ImageSplitExportResult exportSplitImages(const QImage& image, const ImageSplitLayout& layout,
                                         const QString& parentDirectory, const QString& imageName,
                                         std::function<bool()> cancelled,
                                         std::function<void(qint64, qint64)> progress) {
    ImageSplitExportResult result;
    result.totalCount = layout.outputCount();
    const auto cancellationRequested = [&cancelled] { return cancelled && cancelled(); };
    if (cancellationRequested()) {
        setError(result, "Export was canceled.");
        return result;
    }
    if (image.isNull() || image.size() != layout.size || !layout.isValid()) {
        setError(result, "The image or split configuration is invalid.");
        return result;
    }
    try {
        // Conversion happens once on the worker. Already-normalized sources share immutable pixels.
        QImage source = image;
        if (source.colorSpace().isValid() && source.colorSpace() != QColorSpace(QColorSpace::SRgb))
            source = source.convertedToColorSpace(QColorSpace::SRgb);
        if (!source.isNull() && source.format() != QImage::Format_RGBA8888)
            source = source.convertToFormat(QImage::Format_RGBA8888);
        if (source.isNull()) {
            setError(result, "The image could not be prepared for export.");
            return result;
        }
        if (cancellationRequested()) {
            setError(result, "Export was canceled.");
            return result;
        }
        result.directory = createExportDirectory(parentDirectory, imageName);
        if (result.directory.isEmpty()) {
            setError(result, "The output folder could not be created.");
            return result;
        }
        if (progress)
            progress(0, result.totalCount);
        const QDir output(result.directory);
        for (qint64 index = 0; index < result.totalCount; ++index) {
            if (cancellationRequested()) {
                setError(result, "Export was canceled.");
                return result;
            }
            const auto rows = cropRows(source, layout.regionAt(index), cancelled);
            const auto saved = ScreenshotImageFileService::write(
                rows, output.filePath(QStringLiteral("%1.png").arg(index + 1)),
                ScreenshotImageFileFormat::Png);
            if (!saved.succeeded()) {
                if (cancellationRequested())
                    setError(result, "Export was canceled.");
                else
                    setError(result, "Image %1 could not be saved: %2",
                             {QString::number(index + 1), saved.error});
                return result;
            }
            ++result.savedCount;
            if (progress)
                progress(result.savedCount, result.totalCount);
        }
    } catch (const std::bad_alloc&) {
        setError(result, "There is not enough memory to export the image.");
    }
    return result;
}

} // namespace snow_shot::presentation

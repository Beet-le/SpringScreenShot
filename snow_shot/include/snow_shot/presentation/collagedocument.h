// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEDOCUMENT_H
#define SNOW_SHOT_PRESENTATION_COLLAGEDOCUMENT_H

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QColor>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

#include <functional>
#include <memory>

namespace snow_shot::presentation {

enum class CollageDirection { Vertical, Horizontal, Custom };
enum class CollageSizing { Original, Uniform };

struct CollageSettings final {
    CollageDirection direction = CollageDirection::Vertical;
    CollageSizing sizing = CollageSizing::Uniform;
    int border = 0;
    int spacing = 0;
    QColor background = Qt::white;
    int rows = 1;
    int columns = 1;
    bool customGridInitialized = false;

    friend bool operator==(const CollageSettings&, const CollageSettings&) = default;
};

struct CollageItem final {
    quint64 id = 0;
    quint64 revision = 0;
    QImage image;
    QImage preview;
    QTransform transform;
    QString label;
    std::shared_ptr<void> resourceOwner;
};

struct CollageLayout final {
    QSize size;
    QVector<QRect> rects;
    QString error;

    [[nodiscard]] bool isValid() const {
        return size.isValid() && !size.isEmpty() && !rects.isEmpty() && error.isEmpty();
    }
};

[[nodiscard]] QSize transformedSize(const CollageItem& item);
[[nodiscard]] QImage transformedPreview(const CollageItem& item);
[[nodiscard]] CollageLayout calculateCollageLayout(const QVector<CollageItem>& items,
                                                   const CollageSettings& settings);

class CollageDocument final : public QObject {
    Q_OBJECT

  public:
    explicit CollageDocument(QObject* parent = nullptr);
    ~CollageDocument() override;

    [[nodiscard]] const QVector<CollageItem>& items() const;
    [[nodiscard]] CollageSettings settings() const;
    void setSettings(const CollageSettings& settings);
    [[nodiscard]] CollageLayout layout() const;
    [[nodiscard]] QString lastError() const;

    quint64 append(QImage image, QString label, QImage preview = {});
    bool replace(quint64 id, QImage image, QString label, QImage preview = {});
    bool remove(quint64 id);
    void clear();
    bool swapItems(quint64 first, quint64 second);
    bool rotate(quint64 id, bool clockwise);
    bool flip(quint64 id, bool horizontal);
    bool resetTransform(quint64 id);
    [[nodiscard]] quint64 selectedId() const;
    void select(quint64 id);

  signals:
    void changed();
    void selectionChanged();
    void customGridAdjusted(const QStringList& fields);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Immutable originals are shared. Only transformed/resized items allocate new rasters;
// the result owns these rasters and its resource reservation until the last reader retires.
[[nodiscard]] ScreenshotImageRowSource prepareCollageRows(const QVector<CollageItem>& items,
                                                          const CollageSettings& settings,
                                                          std::function<bool()> cancelled = {},
                                                          QString* error = nullptr);

// Full image materialization is deliberately bounded and intended for small verification fixtures.
[[nodiscard]] QImage materializeCollageRows(const ScreenshotImageRowSource& rows,
                                            qsizetype maximumBytes = 16 * 1024 * 1024);

// Reserve native clipboard additions before asking the existing clipboard workflow to prepare
// its PNG and native bitmap representations. Keep this owner until publication finishes.
[[nodiscard]] std::shared_ptr<void> reserveCollageClipboard(QSize outputSize,
                                                            QString* error = nullptr);

struct CollageClipboardEstimate final {
    qint64 bytes = 0;
    qint64 dibBytes = 0;
    qint64 rowScratchBytes = 0;
    QString error;

    [[nodiscard]] bool isValid() const {
        return bytes > 0 && error.isEmpty();
    }
};
[[nodiscard]] CollageClipboardEstimate estimateCollageClipboard(QSize outputSize);

struct CollageResourceUsage final {
    qint64 reservedBytes = 0;
    qint64 limitBytes = 0;
};
[[nodiscard]] CollageResourceUsage collageResourceUsage();

// A single imported image is limited to 64 million pixels. Rendering reserves room for two
// four-byte rasters; use the same admission policy before capturing or importing live pins.
[[nodiscard]] qint64 collageImageImportBytes(QSize size, QString* error = nullptr);

// Admit pixels and painting scratch before capture() allocates its four-byte raster. The returned
// immutable image owns the pixel reservation, including while queued or held by an export worker.
[[nodiscard]] QImage captureCollageImage(QSize size, const std::function<QImage()>& capture,
                                         QString* error = nullptr);

class CollageImportReservation final {
  public:
    CollageImportReservation();
    ~CollageImportReservation();
    bool resize(qint64 bytes, QString* error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
[[nodiscard]] std::shared_ptr<CollageImportReservation>
reserveCollageImport(qint64 bytes, QString* error = nullptr);
[[nodiscard]] std::shared_ptr<void> reserveCollagePreview(qint64 bytes, QString* error = nullptr);

} // namespace snow_shot::presentation

#endif

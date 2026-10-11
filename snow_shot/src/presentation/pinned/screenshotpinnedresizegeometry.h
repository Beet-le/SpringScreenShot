#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStringView>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <optional>

namespace screenshot_pinned_resize_geometry {
enum class DragHandle {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

enum class ScaleAnchor {
    MousePosition,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
};

// Outside-content side strips resize their nearest side. Outside both axes
// selects a corner; the existing inside-content edge band remains available.
[[nodiscard]] std::optional<DragHandle> hitHandle(const QPointF& position, const QRectF& outer,
                                                  const QRectF& content, const QSizeF& edgeBand);

[[nodiscard]] QSize dpiScaledOuterSize(const QSize& pendingOuter, int oldDpi, int newDpi,
                                       int frameMargin);

[[nodiscard]] inline QSize scaledSize(const QSize& baseline, double scale) {
    if (!baseline.isValid() || baseline.isEmpty() || !std::isfinite(scale) || scale <= 0.0) {
        return {};
    }

    return QSize(std::max(1, qRound(baseline.width() * scale)),
                 std::max(1, qRound(baseline.height() * scale)));
}

struct TrackSizeLimits {
    QSize minimum;
    QSize maximum;
};

[[nodiscard]] TrackSizeLimits trackSizeLimits(const QSize& minimum, const QSize& maximum,
                                              const QSize& current, const QSize& target);

[[nodiscard]] inline ScaleAnchor scaleAnchorFromSetting(QStringView value) {
    if (value == u"top_left") {
        return ScaleAnchor::TopLeft;
    }
    if (value == u"top_right") {
        return ScaleAnchor::TopRight;
    }
    if (value == u"bottom_left") {
        return ScaleAnchor::BottomLeft;
    }
    if (value == u"bottom_right") {
        return ScaleAnchor::BottomRight;
    }
    if (value == u"center") {
        return ScaleAnchor::Center;
    }
    return ScaleAnchor::MousePosition;
}

[[nodiscard]] inline QRect anchoredScaleRect(const QRect& reference, const QSize& targetSize,
                                             ScaleAnchor anchor,
                                             const QPointF& mousePosition = QPointF()) {
    if (!reference.isValid() || reference.isEmpty() || !targetSize.isValid() ||
        targetSize.isEmpty()) {
        return {};
    }

    QPointF topLeft;
    switch (anchor) {
    case ScaleAnchor::MousePosition: {
        const double normalizedX = (mousePosition.x() - reference.left()) / reference.width();
        const double normalizedY = (mousePosition.y() - reference.top()) / reference.height();
        topLeft = QPointF(mousePosition.x() - normalizedX * targetSize.width(),
                          mousePosition.y() - normalizedY * targetSize.height());
        break;
    }
    case ScaleAnchor::TopLeft:
        topLeft = QPointF(reference.left(), reference.top());
        break;
    case ScaleAnchor::TopRight:
        topLeft =
            QPointF(reference.left() + reference.width() - targetSize.width(), reference.top());
        break;
    case ScaleAnchor::BottomLeft:
        topLeft =
            QPointF(reference.left(), reference.top() + reference.height() - targetSize.height());
        break;
    case ScaleAnchor::BottomRight:
        topLeft = QPointF(reference.left() + reference.width() - targetSize.width(),
                          reference.top() + reference.height() - targetSize.height());
        break;
    case ScaleAnchor::Center:
        // Truncating the integer size delta keeps opposite zoom steps
        // reversible when old and new dimensions have different parity.
        topLeft = QPointF(reference.left() + (reference.width() - targetSize.width()) / 2,
                          reference.top() + (reference.height() - targetSize.height()) / 2);
        break;
    }
    return QRect(QPoint(qRound(topLeft.x()), qRound(topLeft.y())), targetSize);
}

[[nodiscard]] bool proportionalResizeRect(const QRect& proposed, const QRect& reference,
                                          const QSize& baseline, DragHandle handle,
                                          double minimumScale, double maximumScale, QRect* result,
                                          int frameMargin = 0);

[[nodiscard]] bool dragResizeRect(const QRect& reference, const QPoint& delta,
                                  const QSize& baseline, DragHandle pressed, double minimumScale,
                                  double maximumScale, DragHandle* effective, QRect* result,
                                  int frameMargin = 0);
} // namespace screenshot_pinned_resize_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H

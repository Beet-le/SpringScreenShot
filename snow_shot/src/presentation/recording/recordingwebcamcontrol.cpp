#include "recordingwebcamcontrol.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QtMath>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
class ResizeButton final : public QToolButton {
  public:
    explicit ResizeButton(QWidget* parent) : QToolButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeBDiagCursor);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, isDown() ? 210 : underMouse() ? 180 : 150));
        painter.drawEllipse(rect());
        painter.translate(width() / 2.0, height() / 2.0);
        painter.scale(width() / 32.0, height() / 32.0);
        painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(-6, 6), QPointF(6, -6));
        painter.drawPolyline(QPolygonF{QPointF(0, -6), QPointF(6, -6), QPointF(6, 0)});
        painter.drawPolyline(QPolygonF{QPointF(-6, 0), QPointF(-6, 6), QPointF(0, 6)});
    }
};
} // namespace

RecordingWebcamControl::RecordingWebcamControl(QWidget* parent)
    : QWidget(parent), m_resize(new ResizeButton(this)) {
    setObjectName(QStringLiteral("screenRecordingWebcamControl"));
    m_resize->setObjectName(QStringLiteral("screenRecordingWebcamResize"));
    setAttribute(Qt::WA_NoSystemBackground);
    setCursor(Qt::SizeAllCursor);
    setFocusPolicy(Qt::NoFocus);
    qApp->installEventFilter(this);
    retranslateUi();
    hide();
}

void RecordingWebcamControl::retranslateUi() {
    setAccessibleName(tr("Move webcam"));
    m_resize->setToolTip(tr("Resize webcam"));
    m_resize->setAccessibleName(tr("Resize webcam"));
}

void RecordingWebcamControl::present(const QRectF& bounds, const QSize& region, int size,
                                     const QPoint& offset, const QPoint& fittedOffset,
                                     bool circle) {
    const QRect enclosing = bounds.toAlignedRect();
    m_bounds = bounds.translated(-enclosing.topLeft());
    m_region = region;
    m_size = size;
    m_offset = offset;
    m_fittedOffset = fittedOffset;
    m_circle = circle;
    m_scale = std::min({size, region.width(), region.height()}) / bounds.width();
    setGeometry(enclosing);
    const int side = std::max(1, std::min({32, qFloor(m_bounds.right()) - qCeil(m_bounds.left()),
                                           qFloor(m_bounds.bottom()) - qCeil(m_bounds.top())}));
    m_resize->setGeometry(qFloor(m_bounds.right()) - side, qCeil(m_bounds.top()), side, side);
    const QRegion body(m_bounds.toAlignedRect(), circle ? QRegion::Ellipse : QRegion::Rectangle);
    setMask(body | QRegion(m_resize->geometry()));
    update();
}

void RecordingWebcamControl::paintEvent(QPaintEvent*) {
    // Nonzero alpha retains input on Windows layered windows, including the circle edge.
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 2));
    if (m_circle)
        painter.drawEllipse(m_bounds);
    else
        painter.drawRect(m_bounds);
}

void RecordingWebcamControl::beginInteraction(QMouseEvent* event, bool resize) {
    if (event->button() != Qt::LeftButton || m_interacting || !isVisible() || !isEnabled()) {
        event->ignore();
        return;
    }
    m_interacting = true;
    m_resizing = resize;
    m_changed = false;
    m_initialOffset = m_nextOffset = m_offset;
    m_initialFittedOffset = m_fittedOffset;
    m_initialSize = m_nextSize = m_size;
    m_initialSide = std::min({m_size, m_region.width(), m_region.height()});
    m_initialScale = m_scale;
    m_origin = pointerPosition(event->globalPosition());
    m_resize->setDown(resize);
    grabMouse(resize ? Qt::SizeBDiagCursor : Qt::SizeAllCursor);
    emit interactionChanged();
    event->accept();
}

QPointF RecordingWebcamControl::pointerPosition(const QPointF& globalPosition) const {
#ifdef Q_OS_WIN
    // Qt desktop coordinates change scale across displays. Native pixels keep the
    // gesture continuous even when capture carries the pointer onto another monitor.
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        POINT point{};
        if (GetCursorPos(&point))
            return QPointF(point.x, point.y);
    }
#endif
    // macOS desktop points and offscreen events use the preview's initial transform.
    return globalPosition * m_initialScale;
}

void RecordingWebcamControl::updateInteraction(const QPointF& point) {
    const QPointF delta = pointerPosition(point) - m_origin;
    const qreal resizeDelta = (delta.x() - delta.y()) / 2;
    if (delta.isNull() || (m_resizing && qFuzzyIsNull(resizeDelta))) {
        if (!m_changed)
            return;
        m_nextSize = m_initialSize;
        m_nextOffset = m_initialOffset;
    } else if (m_resizing) {
        m_nextSize = std::clamp(qRound(m_initialSide + resizeDelta), 50, 500);
        m_nextOffset = m_initialFittedOffset;
    } else {
        m_nextOffset = QPoint(std::clamp(qRound(m_initialFittedOffset.x() + delta.x()), 0,
                                         m_region.width() - m_initialSide),
                              std::clamp(qRound(m_initialFittedOffset.y() - delta.y()), 0,
                                         m_region.height() - m_initialSide));
    }
    m_changed = true;
    emit layoutPreviewed(m_nextSize, m_nextOffset);
}

void RecordingWebcamControl::releaseInteraction() {
    m_resize->setDown(false);
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    emit interactionChanged();
}

void RecordingWebcamControl::cancelInteraction() {
    if (!m_interacting)
        return;
    m_interacting = false;
    emit layoutPreviewed(m_initialSize, m_initialOffset);
    releaseInteraction();
}

bool RecordingWebcamControl::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse ||
        (event->type() == QEvent::EnabledChange && !isEnabled()))
        cancelInteraction();
    return QWidget::event(event);
}

bool RecordingWebcamControl::eventFilter(QObject* watched, QEvent* event) {
    if (m_escapeRelease && event->type() == QEvent::KeyRelease &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_escapeRelease = false;
        return true;
    }
    if (m_interacting &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) {
            m_escapeRelease = true;
            cancelInteraction();
        }
        event->accept();
        return true;
    }
    if (m_interacting &&
        (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::WindowBlocked) &&
        watched == window())
        cancelInteraction();
    if (watched == m_resize && event->type() == QEvent::MouseButtonPress) {
        beginInteraction(static_cast<QMouseEvent*>(event), true);
        return event->isAccepted();
    }
    return QWidget::eventFilter(watched, event);
}

void RecordingWebcamControl::mousePressEvent(QMouseEvent* event) {
    beginInteraction(event, false);
}
void RecordingWebcamControl::mouseMoveEvent(QMouseEvent* event) {
    if (!m_interacting)
        return;
    if (event->buttons().testFlag(Qt::LeftButton))
        updateInteraction(event->globalPosition());
    else
        cancelInteraction();
    event->accept();
}
void RecordingWebcamControl::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_interacting || event->button() != Qt::LeftButton)
        return;
    updateInteraction(event->globalPosition());
    m_interacting = false;
    if (m_changed)
        emit layoutCommitted(m_nextSize, m_nextOffset);
    releaseInteraction();
    event->accept();
}

#include <QApplication>
#include <QCoreApplication>
#include <QHelpEvent>
#include <QListView>
#include <QMouseEvent>
#include <QImage>
#include <QPainter>
#include <QProxyStyle>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QStyleOption>
#include <QTest>
#include <QWidget>

#include <algorithm>

#include "widgets/date_picker.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/detail/overlay_popup_controller.h"
#include "widgets/detail/qt_tooltip_bridge.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"

#if defined(Q_OS_MACOS)
#include "macos_native_input.h"
#endif

using adqt::widgets::AdColorPicker;
using adqt::widgets::AdDatePicker;
using adqt::widgets::AdDateRangePicker;
using adqt::widgets::AdPopover;
using adqt::widgets::AdSelect;
using adqt::widgets::AdTooltip;

namespace adqt::widgets::detail {

class OverlayPopupSurfaceTestAccess final {
 public:
  static bool hasPathCache(const OverlayPopupSurface& surface) {
    return surface.pathCache_ != nullptr;
  }

  static bool hasShadowCache(const OverlayPopupSurface& surface) {
    return surface.shadowCache_ != nullptr;
  }

  static bool pathCacheValid(const OverlayPopupSurface& surface) {
    return surface.pathCache_ && surface.pathCache_->valid;
  }

  static bool shadowCacheValid(const OverlayPopupSurface& surface) {
    return surface.shadowCache_ && surface.shadowCache_->valid;
  }
};

}  // namespace adqt::widgets::detail

namespace {

AdSelect::Option makeOption(const QString& value, const QString& label) {
  AdSelect::Option option;
  option.value = value;
  option.label = label;
  return option;
}

QWidget* selectPopupSurface(AdSelect& select) {
  QWidget* candidate = select.view();
  while (candidate && candidate->objectName() != QStringLiteral("adselect-popup")) {
    candidate = candidate->parentWidget();
  }
  return candidate;
}

QWidget* findSurface(const QString& objectName, bool visibleOnly = false) {
  for (QWidget* candidate : QApplication::allWidgets()) {
    if (candidate && candidate->objectName() == objectName &&
        (!visibleOnly || candidate->isVisible())) {
      return candidate;
    }
  }
  return nullptr;
}

void verifyNativeResourcesReleased(QWidget* surface) {
  QVERIFY(surface);
  QTRY_VERIFY(!surface->isVisible());
  QTRY_VERIFY(surface->windowHandle() == nullptr);
  QCOMPARE(surface->backingStore(), nullptr);
}

}  // namespace

class QtToolPopupTest final : public QObject {
  Q_OBJECT

 private slots:
  void customButtonHitAreaIgnoresNativeBevel();
  void popupHoverShapeTracksPaintedSurface();
  void popupSurfacePreservesAntialiasedEdges();
#if defined(Q_OS_MACOS)
  void popupShadowDoesNotCoverUnderlyingWindow();
#endif
  void siblingPopoversReopenAfterOverdueHoverTasks();
  void siblingPopoversReopenAfterScopeRecreation();
  void popoverReleasesAndRecreatesNativeResources();
  void popupTriggerTooltipsRequireOptIn();
  void popupOptionTooltipsRemainVisible();
  void unrelatedLeaveDoesNotDismissTooltip();
  void popupTriggerTooltipsAvoidPopoverAtScreenEdges();
  void popupTriggerTooltipsRequireSpaceBelow();
  void warmTooltipSurvivesGroupPopoverOpening();
  void hoveringFromTooltipToAdjacentPopoverOpensPopover();
  void pendingTooltipSurvivesGroupRouteRegistration();
  void selectReleasesAndRecreatesNativeResources();
  void selectSurvivesPopupContainerDestructionOrder();
  void tooltipReleasesAndRecreatesNativeResources();
  void datePickerReleasesAndRecreatesNativeResources();
  void dateRangePickerReleasesAndRecreatesNativeResources();
  void datePickerSurvivesPopupContainerDestructionOrder();
  void dateRangePickerSurvivesPopupContainerDestructionOrder();
  void popoverSurvivesExternalSurfaceDestruction();
  void popoverFactoryAllowsOwnerDeletion();
  void popoverShowGuardAllowsOwnerDeletion();
  void popoverVisibilityObserverAllowsOwnerDeletion();
  void popoverOpeningPrecedesSurfaceVisibility();
  void popoverOpeningObserverAllowsOwnerDeletion();
  void popoverShowEventAllowsOwnerDeletion();
  void popoverPropertyObserversAllowOwnerDeletion();
  void recreateLifetimeStillDestroysPopupSurface();
  void retainedPopupCachesFollowVisibilityAndStayComponentLocal();
  void retainedFactoryContentIsLazyAndOwnerBound();
  void recreateFactoryContentIsReleasedAfterHide();
  void directContentRemainsCompatibleWithFactoryApi();
  void popoverDestructionAllowsUiEvents();
  void tooltipDestructionAllowsUiEvents();
  void datePickerDestructionAllowsUiEvents();
  void datePickerPopupReplacementAllowsOwnerDeletion();
  void colorPickerPrewarmCanBeDisabled();
  void stationaryPopoverStopsRelayout();
  void visiblePopoverRelayoutDoesNotRaise();
};

void QtToolPopupTest::stationaryPopoverStopsRelayout() {
  using adqt::widgets::detail::OverlayPopupController;
  for (auto layer : {AdPopover::PopupLayerMode::InWindow, AdPopover::PopupLayerMode::QtTool}) {
    QWidget host;
    host.resize(640, 400);
    QPushButton trigger(QStringLiteral("Open"), &host);
    trigger.setGeometry(200, 200, 100, 32);
    AdPopover popover;
    popover.setSourceWidget(&trigger);
    popover.setPopupLayerMode(layer);
    popover.setText(QStringLiteral("Stationary popup"));
    host.show();
    popover.show();
    QVERIFY(popover.isVisible());
    // Allow the bounded opening geometry tail to finish, then require quiescence.
    QTest::qWait(250);
    OverlayPopupController::resetSyncPopupGeometryCountersForTesting();
    QTest::qWait(100);
    QCOMPARE(OverlayPopupController::syncPopupGeometryCallCountForTesting(), 0);
    popover.hide();
  }
}

void QtToolPopupTest::visiblePopoverRelayoutDoesNotRaise() {
  class StackingObserver final : public QObject {
   public:
    int changes = 0;
    bool eventFilter(QObject*, QEvent* event) override {
      if (event->type() == QEvent::ZOrderChange) ++changes;
      return false;
    }
  };
  QWidget host;
  host.resize(800, 600);
  QPushButton trigger(QStringLiteral("Open"), &host);
  trigger.setGeometry(250, 300, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(&trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  auto* content = new QWidget;
  content->setFixedSize(100, 40);
  popover.setContentWidget(content);
  host.show();
  popover.show();
  QTest::qWait(250);
  QWidget* surface = content->window();
  QVERIFY(surface && surface->isVisible());
  StackingObserver observer;
  surface->installEventFilter(&observer);
  const QRect original = surface->geometry();
  content->setFixedSize(180, 80);
  QTRY_VERIFY(surface->size().width() > original.width());
  const QPoint beforeMove = surface->pos();
  trigger.move(trigger.pos() + QPoint(30, 0));
  QTRY_COMPARE(surface->pos(), beforeMove + QPoint(30, 0));
  content->setFixedSize(100, 40);
  QTRY_COMPARE(surface->size(), original.size());
  popover.refreshPopupLayout();
  QTest::qWait(200);
  QCOMPARE(observer.changes, 0);
}

void QtToolPopupTest::customButtonHitAreaIgnoresNativeBevel() {
  class InsetButtonStyle final : public QProxyStyle {
   public:
    QRect subElementRect(SubElement element, const QStyleOption* option,
                         const QWidget* widget = nullptr) const override {
      if (element == SE_PushButtonBevel) {
        return option->rect.adjusted(6, 6, -6, -6);
      }
      return QProxyStyle::subElementRect(element, option, widget);
    }
  };
  adqt::widgets::AdButton button;
  auto* style = new InsetButtonStyle;
  style->setParent(&button);
  button.setStyle(style);
  button.setShape(adqt::widgets::AdButton::Shape::Rounded);
  button.resize(32, 32);
  button.show();
  QSignalSpy clicked(&button, &adqt::widgets::AdButton::clicked);
  int expected = 0;
  for (const QPoint point :
       {QPoint(16, 1), QPoint(16, 16), QPoint(16, 30), QPoint(1, 16), QPoint(30, 16)}) {
    QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(clicked.count(), ++expected);
  }
  button.setShape(adqt::widgets::AdButton::Shape::Circle);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
  QCOMPARE(clicked.count(), expected);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(16, 16));
  QCOMPARE(clicked.count(), ++expected);
  button.setDisabled(true);
  QTest::mouseClick(&button, Qt::LeftButton, Qt::NoModifier, QPoint(16, 16));
  QCOMPARE(clicked.count(), expected);
}

#if defined(Q_OS_MACOS)
void QtToolPopupTest::popupShadowDoesNotCoverUnderlyingWindow() {
  if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
    QSKIP("Requires the Cocoa window server");
  }
  QVERIFY2(macCanPostMouseEvents(),
           "Native click test requires Accessibility event-posting access");
  MacCursorRestore restoreCursor;
  using namespace adqt::widgets::detail;
  QWidget host;
  host.resize(500, 300);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* underlying = new QPushButton(QStringLiteral("Underlying"), &host);
  underlying->setGeometry(host.rect());
  QSignalSpy clicked(underlying, &QPushButton::clicked);
  host.show();
  host.raise();
  macRaiseTestWindow(&host, 1100);
  macActivateApplication();
  host.activateWindow();
  QTRY_VERIFY(host.isActiveWindow());
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  surface.setAttribute(Qt::WA_ShowWithoutActivating);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  auto* option = new QPushButton(QStringLiteral("Popup option"), surface.bodyWidget());
  option->setGeometry(surface.bodyWidget()->rect().adjusted(16, 16, -16, -16));
  QSignalSpy optionClicked(option, &QPushButton::clicked);
  surface.move(host.mapToGlobal(QPoint(80, 60)));
  syncTopLevelToolTransientParent(&surface, &host);
  int expectedUnderlyingClicks = 0;
  int expectedOptionClicks = 0;
  for (int cycle = 0; cycle < 3; ++cycle) {
    surface.setArrowVisible(cycle < 2);
    surface.show();
    surface.raise();
    macRaiseTestWindow(&surface, 1101);
    for (const auto placement : {OverlayPopupPlacement::Top, OverlayPopupPlacement::Bottom,
                                 OverlayPopupPlacement::Left, OverlayPopupPlacement::Right}) {
      surface.setPlacement(placement);
      surface.setArrowCenter(80);
      option->setGeometry(surface.bodyWidget()->rect().adjusted(16, 16, -16, -16));
      const QPoint body = option->mapToGlobal(option->rect().center());
      QCOMPARE(surface.shadowMargins(), QMargins());
      QVERIFY(!surface.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
      QTRY_VERIFY(macWindowReceivesPoint(&surface, body));
      QVERIFY(macWindowHasShadow(&surface));
      // Test real delivery outside the frame, in a rounded corner, and in the
      // transparent strip beside the arrow. No mouse events are replayed.
      const QPoint gutter =
          placement == OverlayPopupPlacement::Top      ? QPoint(20, surface.height() - 2)
          : placement == OverlayPopupPlacement::Bottom ? QPoint(20, 1)
          : placement == OverlayPopupPlacement::Left   ? QPoint(surface.width() - 2, 20)
                                                       : QPoint(1, 20);
      QVector<QPoint> transparentPoints{QPoint(surface.width() / 2, surface.height() + 4),
                                        QPoint(0, 0)};
      if (surface.arrowVisible()) {
        transparentPoints.push_back(gutter);
      }
      for (const QPoint local : transparentPoints) {
        const QPoint point = surface.mapToGlobal(local);
        macPostClick(point);
        ++expectedUnderlyingClicks;
        QTRY_VERIFY2(
            clicked.count() == expectedUnderlyingClicks,
            qPrintable(
                QStringLiteral(
                    "click did not reach underlying button: placement %1, local %2,%3, cycle %4")
                    .arg(static_cast<int>(placement))
                    .arg(local.x())
                    .arg(local.y())
                    .arg(cycle)));
      }
      macPostClick(body);
      ++expectedOptionClicks;
      QTRY_COMPARE(optionClicked.count(), expectedOptionClicks);
      QCOMPARE(clicked.count(), expectedUnderlyingClicks);
    }
    surface.hide();
    surface.releaseTopLevelToolResources();
    syncTopLevelToolTransientParent(&surface, &host);
  }
}
#endif

void QtToolPopupTest::popupSurfacePreservesAntialiasedEdges() {
#if defined(Q_OS_MACOS)
  using namespace adqt::widgets::detail;
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  style.borderColor = Qt::black;
  style.metrics.borderRadius = 12;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  surface.show();
  const auto verifyRendering = [&surface]() {
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
      const auto render = [&surface, dpr](QWidget::RenderFlags flags) {
        QImage image(QSize(qRound(surface.width() * dpr), qRound(surface.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        surface.render(&painter, QPoint(), QRegion(), flags);
        return image;
      };
      const QImage expected = render(QWidget::DrawChildren | QWidget::IgnoreMask);
      const QImage actual = render(QWidget::DrawChildren);
      int blendedPixels = 0;
      for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
          const int alpha = qAlpha(expected.pixel(x, y));
          blendedPixels += alpha > 0 && alpha < 255;
        }
      }
      if (surface.arrowVisible() || surface.surfaceStyle().metrics.borderRadius > 0) {
        QVERIFY(blendedPixels > 0);
      }
      QVERIFY2(
          actual == expected,
          qPrintable(
              QStringLiteral("native surface clipping loses painted edges at DPR %1").arg(dpr)));
    }
  };
  for (const auto placement : {OverlayPopupPlacement::Top, OverlayPopupPlacement::Bottom,
                               OverlayPopupPlacement::Left, OverlayPopupPlacement::Right}) {
    surface.setPlacement(placement);
    for (const qreal center : {42.0, 83.5}) {
      surface.setArrowCenter(center);
      for (const int borderWidth : {0, 1, 3}) {
        style.metrics.borderWidth = borderWidth;
        style.arrowBackground = QColor(230, 240, 255);
        style.arrowBorderColor = QColor(30, 60, 90);
        surface.setSurfaceStyle(style);
        verifyRendering();
      }
    }
    surface.resize(surface.width() + 10, surface.height() + 10);
    verifyRendering();
  }
  surface.setArrowVisible(false);
  verifyRendering();
  style.metrics.borderRadius = 0;
  surface.setSurfaceStyle(style);
  verifyRendering();
  surface.hide();
  surface.releaseTopLevelToolResources();
  surface.setArrowVisible(true);
  surface.show();
  verifyRendering();
  QWidget parent;
  parent.resize(500, 300);
  surface.hide();
  surface.setParent(&parent);
  parent.show();
  surface.show();
  verifyRendering();
  surface.hide();
  surface.setParent(nullptr, overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.show();
  verifyRendering();
#endif
}

void QtToolPopupTest::popupHoverShapeTracksPaintedSurface() {
#if defined(Q_OS_MACOS)
  using namespace adqt::widgets::detail;
  OverlayPopupSurface surface;
  surface.setWindowFlags(overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.setAttribute(Qt::WA_TranslucentBackground);
  OverlayPopupSurfaceStyle style;
  style.background = Qt::white;
  style.metrics.borderRadius = 12;
  surface.setSurfaceStyle(style);
  surface.resize(240, 120);
  const auto verifyShape = [&surface]() {
    QImage image(surface.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    surface.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();
    QVERIFY(surface.containsInteractiveLocalPos(surface.bodyWidget()->geometry().center()));
    QCOMPARE(surface.shadowMargins(), QMargins());
    // Sample away from rasterized edges: input must follow the bubble and arrow,
    // never the surrounding painted-shadow frame.
    for (int y = 2; y < surface.height(); y += 5) {
      for (int x = 2; x < surface.width(); x += 5) {
        const QPoint point(x, y);
        const bool inside = surface.containsInteractiveLocalPos(point);
        bool awayFromEdge = true;
        for (int dy = -2; dy <= 2; ++dy) {
          for (int dx = -2; dx <= 2; ++dx) {
            awayFromEdge &= inside == surface.containsInteractiveLocalPos(point + QPoint(dx, dy));
          }
        }
        if (awayFromEdge) {
          QVERIFY2((qAlpha(image.pixel(point)) > 0) == inside,
                   qPrintable(QStringLiteral("shape mismatch at %1,%2").arg(x).arg(y)));
        }
      }
    }
  };
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
  for (auto placement : {OverlayPopupPlacement::Bottom, OverlayPopupPlacement::Left,
                         OverlayPopupPlacement::Right, OverlayPopupPlacement::Top}) {
    surface.setPlacement(placement);
    surface.setArrowCenter(42);
    verifyShape();
    surface.resize(surface.width() + 20, surface.height() + 10);
    verifyShape();
  }
  surface.setArrowVisible(false);
  verifyShape();
  style.metrics.borderRadius = 0;
  surface.setSurfaceStyle(style);
  verifyShape();
  surface.hide();
  surface.releaseTopLevelToolResources();
  surface.setArrowVisible(true);
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
  QWidget parent;
  parent.resize(500, 300);
  surface.hide();
  surface.setParent(&parent);
  parent.show();
  surface.show();
  QCoreApplication::processEvents();
  QVERIFY(surface.mask().isEmpty());
  surface.setParent(nullptr, overlayPopupSurfaceWindowFlags(adqt::widgets::adQtToolWindowFlags()));
  surface.show();
  QCoreApplication::processEvents();
  verifyShape();
#endif
}

void QtToolPopupTest::unrelatedLeaveDoesNotDismissTooltip() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(400, 300);
  QPushButton target(QStringLiteral("Target"), &host);
  target.setGeometry(30, 30, 100, 30);
  target.setToolTip(QStringLiteral("Target help"));
  QWidget unrelated(&host);
  unrelated.setGeometry(250, 200, 50, 50);
  host.show();
  host.activateWindow();
  QCoreApplication::processEvents();
  const QPoint local = target.rect().center();
  QHelpEvent help(QEvent::ToolTip, local, target.mapToGlobal(local));
  QApplication::sendEvent(&target, &help);
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(&unrelated, &leave);
  QTest::qWait(400);
  QVERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  target.hide();
  QTRY_VERIFY(!findSurface(QStringLiteral("adtooltip-surface"), true));
}

void QtToolPopupTest::popupOptionTooltipsRemainVisible() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(640, 360);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(240, 160, 100, 32);
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover(trigger);
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  auto* option = new QPushButton(QStringLiteral("Rectangle"));
  option->setToolTip(QStringLiteral("Draw rectangle"));
  popover.setContentWidget(option);
  host.show();
  QCoreApplication::processEvents();
  popover.show();
  QCoreApplication::processEvents();
  QVERIFY(option->isVisible());
  host.activateWindow();
  QCoreApplication::processEvents();
  QVERIFY(!option->window()->isActiveWindow());
  const QPoint center = option->rect().center();
  QTest::mouseMove(option->window()->windowHandle(), option->mapTo(option->window(), center));
  QTRY_VERIFY_WITH_TIMEOUT(findSurface(QStringLiteral("adtooltip-surface"), true), 2000);
  QVERIFY(popover.isVisible());
  QVERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  bool matched = false;
  for (auto* tip : qApp->findChildren<AdTooltip*>()) {
    matched |=
        tip->isVisible() && tip->targetWidget() == option && tip->text() == option->toolTip();
  }
  QVERIFY(matched);
}

void QtToolPopupTest::popupTriggerTooltipsRequireOptIn() {
  AdTooltip::installApplicationTooltips();
  QWidget host;
  host.resize(640, 360);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setText(QStringLiteral("Drawing options"));
  popover.setPlacement(AdPopover::Placement::Top);
  host.show();
  QCoreApplication::processEvents();
  popover.show();
  QCoreApplication::processEvents();
  QVERIFY(popover.isVisible());

  for (bool enabled : {false, true, false}) {
    trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, enabled);
    const QPoint center = trigger->rect().center();
    QHelpEvent help(QEvent::ToolTip, center, trigger->mapToGlobal(center));
    QApplication::sendEvent(trigger, &help);
    QCOMPARE(help.isAccepted(), enabled);
    bool visible = false;
    for (auto* tooltip : qApp->findChildren<AdTooltip*>()) {
      visible |= tooltip->isVisible() && tooltip->targetWidget() == trigger &&
                 tooltip->text() == trigger->toolTip();
    }
    QCOMPARE(visible, enabled);
  }
}

void QtToolPopupTest::popupTriggerTooltipsAvoidPopoverAtScreenEdges() {
  AdTooltip::installApplicationTooltips();
  const QRect screen = QApplication::primaryScreen()->availableGeometry();
  for (int position : {0, 1, 2}) {
    QWidget host(nullptr, Qt::FramelessWindowHint);
    const int y = position == 0   ? screen.top() + 2
                  : position == 1 ? screen.center().y()
                                  : screen.bottom() - 41;
    host.setGeometry(screen.center().x() - 160, y, 320, 40);
    auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
    trigger->setGeometry(100, 8, 100, 32);
    trigger->setToolTip(QStringLiteral("Draw (2)"));
    trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
    AdPopover popover;
    popover.setSourceWidget(trigger);
    popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popover.setPlacement(AdPopover::Placement::Top);
    auto* content = new QWidget;
    content->setFixedSize(260, 90);
    popover.setContentWidget(content);
    host.show();
    QCoreApplication::processEvents();
    const auto requestTooltip = [trigger]() {
      const QPoint center = trigger->rect().center();
      QHelpEvent help(QEvent::ToolTip, center, trigger->mapToGlobal(center));
      QApplication::sendEvent(trigger, &help);
      for (auto* tip : qApp->findChildren<AdTooltip*>()) {
        if (tip->isVisible() && tip->targetWidget() == trigger) {
          return tip;
        }
      }
      return static_cast<AdTooltip*>(nullptr);
    };
    // Bottom-only placement applies before the group menu opens as well.
    QCOMPARE(requestTooltip() != nullptr, position != 2);
    popover.show();
    QCoreApplication::processEvents();
    QVERIFY(popover.isVisible());
    auto* popup = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(content->window());
    const QRect popupBody =
        QRect(popup->mapToGlobal(QPoint()), popup->size()).marginsRemoved(popup->shadowMargins());
    const QRect triggerRect(trigger->mapToGlobal(QPoint()), trigger->size());
    QCOMPARE(popupBody.center().y() > triggerRect.center().y(), position == 0);
    AdTooltip* tip = requestTooltip();
    QCOMPARE(tip != nullptr, position == 1);
    if (tip) {
      QCOMPARE(tip->anchorWidget(), trigger);
      QCOMPARE(tip->anchorRect(), trigger->rect());
      QCOMPARE(tip->placement(), AdTooltip::Placement::Bottom);
      auto* surface = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(
          findSurface(QStringLiteral("adtooltip-surface"), true));
      QVERIFY(surface && surface->isVisible());
      const QRect body = QRect(surface->mapToGlobal(QPoint()), surface->size())
                             .marginsRemoved(surface->shadowMargins());
      QVERIFY(body.top() > triggerRect.bottom());
      QVERIFY(!body.intersects(popupBody));
      QVERIFY(screen.contains(body));
    }
    popover.hide();
    QCoreApplication::processEvents();
    QCOMPARE(requestTooltip() != nullptr, position != 2);
  }
}

void QtToolPopupTest::popupTriggerTooltipsRequireSpaceBelow() {
  AdTooltip::installApplicationTooltips();
  const QRect screen = QApplication::primaryScreen()->availableGeometry();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.setGeometry(screen.center().x() - 100, screen.center().y(), 200, 32);
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(50, 0, 100, 32);
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  host.show();
  QCoreApplication::processEvents();
  AdTooltip::showText(trigger, QStringLiteral("Draw (2)"));
  auto* surface = static_cast<adqt::widgets::detail::OverlayPopupSurface*>(
      findSurface(QStringLiteral("adtooltip-surface"), true));
  QVERIFY(surface && surface->isVisible());
  const QRect body = QRect(surface->mapToGlobal(QPoint()), surface->size())
                         .marginsRemoved(surface->shadowMargins());
  const int requiredSpace = body.bottom() - trigger->mapToGlobal(trigger->rect().bottomLeft()).y();
  for (int extraSpace : {0, -1, 1}) {
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      tip->hide();
    }
    const int triggerBottom = screen.bottom() - requiredSpace - extraSpace;
    host.move(host.x(), triggerBottom - trigger->height() + 1);
    QCoreApplication::processEvents();
    AdTooltip::showText(trigger, QStringLiteral("Draw (2)"));
    bool visible = false;
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      visible |= tip->isVisible() && tip->targetWidget() == trigger;
    }
    QCOMPARE(visible, extraSpace >= 0);
  }
}

void QtToolPopupTest::warmTooltipSurvivesGroupPopoverOpening() {
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 100);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* previous = new QPushButton(QStringLiteral("Previous"), &host);
  previous->setGeometry(20, 20, 100, 32);
  previous->setToolTip(QStringLiteral("Previous tool"));
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(140, 20, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  popover.setTriggers(AdPopover::Trigger::Hover);
  auto* content = new QWidget;
  content->setFixedSize(240, 60);
  popover.setContentWidget(content);
  host.show();
  QCoreApplication::processEvents();

  const auto requestTooltip = [](QWidget* target) {
    const QPoint center = target->rect().center();
    QHelpEvent help(QEvent::ToolTip, center, target->mapToGlobal(center));
    QApplication::sendEvent(target, &help);
  };
  requestTooltip(previous);
  AdTooltip* tip = nullptr;
  for (auto* candidate : qApp->findChildren<AdTooltip*>()) {
    if (candidate->isVisible() && candidate->targetWidget() == previous) {
      tip = candidate;
      break;
    }
  }
  QVERIFY(tip);
  QSignalSpy visibility(tip, &AdTooltip::visibleChanged);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(previous, &leave);
  requestTooltip(trigger);
  QVERIFY(tip->isVisible());
  QCOMPARE(tip->targetWidget(), trigger);
  QCOMPARE(tip->text(), trigger->toolTip());
  for (int cycle = 0; cycle < 3; ++cycle) {
    popover.show();
    QVERIFY(popover.isVisible());
    const auto* surface =
        static_cast<const adqt::widgets::detail::OverlayPopupSurface*>(content->window());
    const QRect body = surface->rect().marginsRemoved(surface->shadowMargins());
    QVERIFY(surface->mapToGlobal(body.bottomLeft()).y() < trigger->mapToGlobal(QPoint()).y());
    QVERIFY2(tip->isVisible(), "opening the upper group popover must preserve its visible tip");
    QCOMPARE(tip->targetWidget(), trigger);
    QCOMPARE(tip->text(), trigger->toolTip());
    popover.hide();
    QVERIFY(tip->isVisible());
  }
  QCOMPARE(visibility.count(), 0);
  tip->hide();
}

void QtToolPopupTest::hoveringFromTooltipToAdjacentPopoverOpensPopover() {
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 120);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* previous = new QPushButton(QStringLiteral("Scroll"), &host);
  previous->setGeometry(80, 55, 100, 32);
  previous->setToolTip(QStringLiteral("Scrolling screenshot"));
  auto* trigger = new QPushButton(QStringLiteral("Save"), &host);
  trigger->setGeometry(180, 55, 100, 32);
  trigger->setToolTip(QStringLiteral("Save as file"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPlacement(AdPopover::Placement::Top);
  popover.setTriggers(AdPopover::Trigger::Hover);
  popover.setHoverOpenDelayMs(30);
  auto* content = new QWidget;
  content->setFixedSize(160, 40);
  popover.setContentWidget(content);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();

  QTest::mouseMove(previous, previous->rect().center());
  AdTooltip::showText(previous, previous->toolTip());
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QTest::mouseMove(trigger, trigger->rect().center());
  AdTooltip::showText(trigger, trigger->toolTip());
  QTRY_VERIFY(findSurface(QStringLiteral("adtooltip-surface"), true));
  QTRY_VERIFY(popover.isVisible());
  QTRY_VERIFY(content->window()->isVisible());
}

void QtToolPopupTest::pendingTooltipSurvivesGroupRouteRegistration() {
  class TooltipDelayStyle final : public QProxyStyle {
   public:
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* data = nullptr) const override {
      return hint == QStyle::SH_ToolTip_WakeUpDelay
                 ? 100
                 : QProxyStyle::styleHint(hint, option, widget, data);
    }
  };
  AdTooltip::installApplicationTooltips();
  QWidget host(nullptr, Qt::FramelessWindowHint);
  host.resize(400, 100);
  host.move(QApplication::primaryScreen()->availableGeometry().center() - host.rect().center());
  auto* trigger = new QPushButton(QStringLiteral("Draw"), &host);
  trigger->setGeometry(140, 20, 100, 32);
  trigger->setToolTip(QStringLiteral("Draw (2)"));
  trigger->setProperty(adqt::widgets::detail::kPopupTriggerTooltipEnabledProperty, true);
  auto* style = new TooltipDelayStyle;
  style->setParent(trigger);
  trigger->setStyle(style);
  // Exercise route registration separately from platform focus changes when
  // opening native windows. The route's surface is already laid out above the button.
  QWidget popup(nullptr, Qt::Tool | Qt::FramelessWindowHint);
  popup.setGeometry(trigger->mapToGlobal(QPoint(0, -80)).x(),
                    trigger->mapToGlobal(QPoint(0, -80)).y(), 180, 60);
  host.show();
  popup.show();
  QCoreApplication::processEvents();
  const QPoint center = trigger->rect().center();
  const QPoint global = trigger->mapToGlobal(center);
  QMouseEvent move(QEvent::MouseMove, center, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(trigger, &move);
  QHelpEvent help(QEvent::ToolTip, center, global);
  QApplication::sendEvent(trigger, &help);
  QVERIFY(help.isAccepted());
  const auto visibleTip = [trigger]() -> AdTooltip* {
    for (auto* tip : qApp->findChildren<AdTooltip*>()) {
      if (tip->isVisible() && tip->targetWidget() == trigger) {
        return tip;
      }
    }
    return nullptr;
  };
  QVERIFY(!visibleTip());
  QObject owner;
  adqt::widgets::detail::syncTopLevelPopupTooltipRoute(&owner, trigger, &popup, true);
  QTRY_VERIFY_WITH_TIMEOUT(visibleTip(), 1500);
  AdTooltip* tip = visibleTip();
  QSignalSpy visibility(tip, &AdTooltip::visibleChanged);
  adqt::widgets::detail::syncTopLevelPopupTooltipRoute(&owner, trigger, &popup, false);
  QVERIFY(tip->isVisible());
  QCOMPARE(visibility.count(), 0);
  tip->hide();
}

void QtToolPopupTest::siblingPopoversReopenAfterOverdueHoverTasks() {
  QWidget host;
  host.resize(640, 360);
  QPushButton first(QStringLiteral("First"), &host), second(QStringLiteral("Second"), &host);
  first.setGeometry(50, 150, 100, 32);
  second.setGeometry(220, 150, 100, 32);
  AdPopover firstPopup, secondPopup;
  firstPopup.setSourceWidget(&first);
  secondPopup.setSourceWidget(&second);
  for (auto* popup : {&firstPopup, &secondPopup}) {
    popup->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popup->setTriggers(AdPopover::Trigger::Hover);
    popup->setHoverOpenDelayMs(10);
    popup->setHoverCloseDelayMs(10);
    auto* content = new QWidget;
    content->setFixedSize(100, 30);
    popup->setContentWidget(content);
  }
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();
  for (int cycle = 0; cycle < 3; ++cycle) {
    QTest::mouseMove(&host, QPoint(400, 280));
    QTRY_VERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QTest::mouseMove(&first, first.rect().center());
    // Let hover deadlines become overdue without dispatching GUI events.
    QTest::qSleep(40);
    QTRY_VERIFY(firstPopup.isVisible());
    QTRY_VERIFY(firstPopup.contentWidget()->window()->isVisible());
    QTest::mouseMove(&second, second.rect().center());
    QTest::qSleep(40);
    QTRY_VERIFY(secondPopup.isVisible());
    QTRY_VERIFY(secondPopup.contentWidget()->window()->isVisible());
    QTRY_VERIFY(!firstPopup.isVisible());
  }
}

void QtToolPopupTest::siblingPopoversReopenAfterScopeRecreation() {
  class ReusableScope : public QWidget {
   public:
    void releaseNativeSurface() {
      hide();
      destroy(true, true);
    }
  } host;
  host.resize(640, 360);
  QPushButton first(QStringLiteral("First"), &host), second(QStringLiteral("Second"), &host);
  first.setGeometry(50, 150, 100, 32);
  second.setGeometry(220, 150, 100, 32);
  AdPopover firstPopup, secondPopup;
  firstPopup.setSourceWidget(&first);
  secondPopup.setSourceWidget(&second);
  for (auto* popup : {&firstPopup, &secondPopup}) {
    popup->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popup->setTriggers(AdPopover::Trigger::Hover);
    popup->setHoverOpenDelayMs(40);
    popup->setHoverCloseDelayMs(10);
    auto* content = new QWidget;
    content->setFixedSize(100, 30);
    popup->setContentWidget(content);
  }
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  host.raise();
  for (bool closeBeforeHoverDeadline : {true, false}) {
    QTest::mouseMove(&host, QPoint(400, 280));
    QTRY_VERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QTest::mouseMove(&first, first.rect().center());
    if (closeBeforeHoverDeadline) {
      QVERIFY(!firstPopup.isVisible());
    } else {
      QTRY_VERIFY(firstPopup.isVisible());
      QVERIFY(firstPopup.contentWidget()->window()->isVisible());
    }
    // Capture exit retains the QObject tree but releases the toolbar's native window.
    host.releaseNativeSurface();
    QTest::qWait(80);
    QVERIFY(!firstPopup.isVisible() && !secondPopup.isVisible());
    QVERIFY(!host.windowHandle());
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.raise();
    QTest::mouseMove(&host, QPoint(400, 280));
    for (auto* popup : {&secondPopup, &firstPopup}) {
      QWidget* trigger = popup->sourceWidget();
      QTest::mouseMove(trigger, trigger->rect().center());
      QTRY_VERIFY(popup->isVisible());
      QVERIFY(popup->contentWidget()->isVisible());
      QVERIFY(popup->contentWidget()->window()->isVisible());
    }
  }
}

void QtToolPopupTest::popoverReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  popover.hide();
  verifyNativeResourcesReleased(surface);

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adpopover-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  // A close followed by an immediate reopen must cancel the deferred release.
  popover.hide();
  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCoreApplication::processEvents();
  QVERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::selectSurvivesPopupContainerDestructionOrder() {
  auto* host = new QWidget;
  auto* select = new AdSelect(host);
  select->setOptions({makeOption(QStringLiteral("hsb"), QStringLiteral("HSB"))});
  host->show();
  select->showPopup();
  QPointer<QWidget> surface = selectPopupSurface(*select);
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), host);
  select->hidePopup();

  // A picker reparents its format selector into a later-created panel. The
  // original container then destroys the popup before the selector itself.
  auto* panel = new QWidget(host);
  select->setParent(panel);
  QPointer<AdSelect> guardedSelect = select;
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(guardedSelect.isNull());

  QWidget survivingHost;
  host = new QWidget;
  select = new AdSelect(host);
  select->setOptions({makeOption(QStringLiteral("rgb"), QStringLiteral("RGB"))});
  host->show();
  select->showPopup();
  surface = selectPopupSurface(*select);
  QVERIFY(surface);
  select->hidePopup();
  select->setParent(&survivingHost);
  delete host;
  QVERIFY(surface.isNull());
  survivingHost.show();
  select->show();
  select->showPopup();
  QVERIFY(selectPopupSurface(*select));
  QVERIFY(select->popupVisible());
}

void QtToolPopupTest::selectReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdSelect select(&host);
  select.setGeometry(24, 24, 180, 32);
  select.setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
  select.setOptions({makeOption(QStringLiteral("one"), QStringLiteral("One")),
                     makeOption(QStringLiteral("two"), QStringLiteral("Two"))});

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QWidget* surface = selectPopupSurface(select);
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  select.hidePopup();
  verifyNativeResourcesReleased(surface);

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QCOMPARE(selectPopupSurface(select), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::tooltipReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* target = new QPushButton(QStringLiteral("Target"), &host);
  target->setGeometry(24, 24, 100, 32);

  AdTooltip tooltip;
  tooltip.setTargetWidget(target);
  tooltip.setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
  tooltip.setTriggers(AdTooltip::Trigger::Click);
  tooltip.setText(QStringLiteral("Tooltip content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adtooltip-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  tooltip.hide();
  verifyNativeResourcesReleased(surface);

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adtooltip-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::datePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdDatePicker picker(&host);
  picker.setGeometry(24, 24, 220, 32);
  picker.setPopupLayerMode(AdDatePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addatepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::dateRangePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(760, 360);
  AdDateRangePicker picker(&host);
  picker.setGeometry(24, 24, 320, 32);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addaterangepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::datePickerSurvivesPopupContainerDestructionOrder() {
  auto* host = new QWidget;
  host->resize(640, 360);
  auto* picker = new AdDatePicker(host);
  picker->setGeometry(24, 24, 220, 32);
  host->show();
  picker->showPopup();
  QPointer<QWidget> surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), host);
  picker->hidePopup();
  picker->raise();
  QVERIFY(host->children().indexOf(surface) < host->children().indexOf(picker));
  QPointer<AdDatePicker> guardedPicker = picker;
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(guardedPicker.isNull());

  QWidget survivingHost;
  survivingHost.resize(640, 360);
  host = new QWidget;
  host->resize(640, 360);
  picker = new AdDatePicker(host);
  picker->setGeometry(24, 24, 220, 32);
  host->show();
  picker->showPopup();
  surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  picker->hidePopup();
  picker->setParent(&survivingHost);
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(!picker->popupVisible());
  picker->setDate(QDate(2026, 10, 11));
  survivingHost.show();
  picker->show();
  picker->showPopup();
  surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), &survivingHost);
  QVERIFY(picker->popupVisible());
  QCOMPARE(picker->date(), QDate(2026, 10, 11));

  delete surface.data();
  QVERIFY(surface.isNull());
  QVERIFY(!picker->popupVisible());
  picker->setDate(QDate(2026, 10, 12));
  picker->showPopup();
  surface = findSurface(QStringLiteral("addatepicker-popup"), true);
  QVERIFY(surface);
  picker->clearPopupContentWrapperFactory();
  QPointer<QWidget> replacement = findSurface(QStringLiteral("addatepicker-popup"), true);
  QVERIFY(replacement);
  QVERIFY(replacement != surface);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(surface.isNull());
  QVERIFY(replacement);
  QVERIFY(picker->popupVisible());
  picker->setDate(QDate(2026, 10, 13));
  QCOMPARE(picker->date(), QDate(2026, 10, 13));
}

void QtToolPopupTest::dateRangePickerSurvivesPopupContainerDestructionOrder() {
  auto* host = new QWidget;
  host->resize(760, 360);
  auto* picker = new AdDateRangePicker(host);
  picker->setGeometry(24, 24, 320, 32);
  host->show();
  picker->showPopup();
  QPointer<QWidget> surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), host);
  picker->hidePopup();
  picker->raise();
  QVERIFY(host->children().indexOf(surface) < host->children().indexOf(picker));
  QPointer<AdDateRangePicker> guardedPicker = picker;
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(guardedPicker.isNull());

  QWidget survivingHost;
  survivingHost.resize(760, 360);
  host = new QWidget;
  host->resize(760, 360);
  picker = new AdDateRangePicker(host);
  picker->setGeometry(24, 24, 320, 32);
  host->show();
  picker->showPopup();
  surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  picker->hidePopup();
  picker->setParent(&survivingHost);
  delete host;
  QVERIFY(surface.isNull());
  QVERIFY(!picker->popupVisible());
  picker->setRange(QDate(2026, 10, 11), QDate(2026, 10, 12));
  survivingHost.show();
  picker->show();
  picker->showPopup();
  surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  QCOMPARE(surface->parentWidget(), &survivingHost);
  QVERIFY(picker->popupVisible());
  QCOMPARE(picker->startDate(), QDate(2026, 10, 11));
  QCOMPARE(picker->endDate(), QDate(2026, 10, 12));

  delete surface.data();
  QVERIFY(surface.isNull());
  QVERIFY(!picker->popupVisible());
  picker->setRange(QDate(2026, 10, 12), QDate(2026, 10, 13));
  picker->showPopup();
  surface = findSurface(QStringLiteral("addaterangepicker-popup"), true);
  QVERIFY(surface);
  picker->clearPopupContentWrapperFactory();
  QPointer<QWidget> replacement = findSurface(QStringLiteral("addaterangepicker-popup"), true);
  QVERIFY(replacement);
  QVERIFY(replacement != surface);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(surface.isNull());
  QVERIFY(replacement);
  QVERIFY(picker->popupVisible());
  picker->setRange(QDate(2026, 10, 13), QDate(2026, 10, 14));
  QCOMPARE(picker->startDate(), QDate(2026, 10, 13));
  QCOMPARE(picker->endDate(), QDate(2026, 10, 14));
}

void QtToolPopupTest::popoverSurvivesExternalSurfaceDestruction() {
  QWidget host;
  host.resize(640, 360);
  QPushButton trigger(QStringLiteral("Open"), &host);
  trigger.setGeometry(24, 24, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(&trigger);
  popover.setText(QStringLiteral("Fallback content"));
  QPointer<QWidget> title = new QPushButton(QStringLiteral("Title"));
  QPointer<QWidget> content = new QPushButton(QStringLiteral("Content"));
  popover.setTitleWidget(title);
  popover.setContentWidget(content);
  host.show();
  popover.show();
  QPointer<QWidget> surface = findSurface(QStringLiteral("adpopover-surface"), true);
  QVERIFY(surface);
  QVERIFY(surface->isAncestorOf(title));
  QVERIFY(surface->isAncestorOf(content));

  delete surface.data();
  QVERIFY(surface.isNull());
  QVERIFY(!popover.isVisible());
  QVERIFY(title);
  QVERIFY(content);
  QCOMPARE(popover.titleWidget(), title.data());
  QCOMPARE(popover.contentWidget(), content.data());
  popover.show();
  QPointer<QWidget> replacement = findSurface(QStringLiteral("adpopover-surface"), true);
  QVERIFY(replacement);
  QVERIFY(replacement->isAncestorOf(title));
  QVERIFY(replacement->isAncestorOf(content));
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  QPointer<QWidget> latest = findSurface(QStringLiteral("adpopover-surface"), true);
  QVERIFY(latest);
  QVERIFY(latest != replacement);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QVERIFY(replacement.isNull());
  QVERIFY(latest);
  QVERIFY(popover.isVisible());
  QVERIFY(latest->isAncestorOf(title));
  QVERIFY(latest->isAncestorOf(content));
}

void QtToolPopupTest::popoverFactoryAllowsOwnerDeletion() {
  for (int entryPoint = 0; entryPoint < 5; ++entryPoint) {
    QWidget host;
    host.resize(640, 360);
    QPushButton trigger(QStringLiteral("Open"), &host);
    trigger.setGeometry(24, 24, 100, 32);
    QPointer<AdPopover> popover = new AdPopover;
    popover->setSourceWidget(&trigger);
    popover->setTriggers(AdPopover::Trigger::Click);
    if (entryPoint == 2) {
      popover->setVisibilityPolicy(AdPopover::VisibilityPolicy::Manual);
    }
    if (entryPoint == 4) {
      connect(popover, &AdPopover::contentWidgetChanged, &host, [&](QWidget* content) {
        if (content) {
          delete popover.data();
        }
      });
    }
    int creations = 0;
    popover->setContentFactory([&]() -> QWidget* {
      ++creations;
      if (entryPoint == 4) {
        return new QWidget;
      }
      delete popover.data();
      return nullptr;
    });
    host.show();
    if (entryPoint == 0) {
      popover->preparePopup();
    } else if (entryPoint == 3) {
      QTest::mouseClick(&trigger, Qt::LeftButton);
    } else {
      popover->show();
    }
    QCOMPARE(creations, 1);
    QVERIFY(popover.isNull());
    QTest::mouseClick(&trigger, Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

void QtToolPopupTest::popoverShowGuardAllowsOwnerDeletion() {
  QWidget host;
  host.resize(640, 360);
  QPushButton trigger(QStringLiteral("Open"), &host);
  trigger.setGeometry(24, 24, 100, 32);
  QPointer<AdPopover> popover = new AdPopover;
  popover->setSourceWidget(&trigger);
  popover->setText(QStringLiteral("Guarded popup"));
  int checks = 0;
  popover->setSurfaceShowGuard([&](QWidget*) {
    ++checks;
    delete popover.data();
    return true;
  });
  host.show();
  popover->show();
  QCOMPARE(checks, 1);
  QVERIFY(popover.isNull());
  QTest::mouseClick(&trigger, Qt::LeftButton);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void QtToolPopupTest::popoverVisibilityObserverAllowsOwnerDeletion() {
  for (bool deleteOnShow : {false, true}) {
    QWidget host;
    host.resize(640, 360);
    QPushButton trigger(QStringLiteral("Open"), &host);
    trigger.setGeometry(24, 24, 100, 32);
    QPointer<AdPopover> popover = new AdPopover;
    popover->setSourceWidget(&trigger);
    popover->setText(QStringLiteral("Observed popup"));
    connect(popover, &AdPopover::visibleChanged, &host, [&](bool visible) {
      if (visible == deleteOnShow) {
        delete popover.data();
      }
    });
    host.show();
    popover->show();
    if (!deleteOnShow) {
      QVERIFY(popover);
      popover->hide();
    }
    QVERIFY(popover.isNull());
    QTest::mouseClick(&trigger, Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

void QtToolPopupTest::popoverOpeningPrecedesSurfaceVisibility() {
  for (auto layer : {AdPopover::PopupLayerMode::InWindow, AdPopover::PopupLayerMode::QtTool}) {
    QWidget host;
    host.resize(640, 360);
    QPushButton trigger(QStringLiteral("Open"), &host);
    trigger.setGeometry(24, 24, 100, 32);
    AdPopover popover;
    popover.setSourceWidget(&trigger);
    popover.setTriggers(AdPopover::Trigger::Click);
    popover.setPopupLayerMode(layer);
    popover.setText(QStringLiteral("Alignment options"));
    QStringList transitions;
    connect(&popover, &AdPopover::popupOpening, &host, [&] {
      QVERIFY(!popover.surfaceWidget() || !popover.surfaceWidget()->isVisible());
      transitions.append(QStringLiteral("opening"));
    });
    connect(&popover, &AdPopover::visibleChanged, &host, [&](bool visible) {
      transitions.append(visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
    });
    host.show();
    popover.preparePopup();
    QVERIFY(transitions.isEmpty());
    popover.show();
    QCOMPARE(transitions, QStringList({QStringLiteral("opening"), QStringLiteral("visible")}));
    popover.show();
    popover.refreshPopupLayout();
    QCOMPARE(transitions.size(), 2);
    popover.hide();
    transitions.clear();
    QTest::mouseClick(&trigger, Qt::LeftButton);
    QCOMPARE(transitions, QStringList({QStringLiteral("opening"), QStringLiteral("visible")}));
    popover.hide();
    QCOMPARE(transitions.last(), QStringLiteral("hidden"));
  }
}

void QtToolPopupTest::popoverOpeningObserverAllowsOwnerDeletion() {
  QWidget host;
  host.resize(640, 360);
  QPushButton trigger(QStringLiteral("Open"), &host);
  trigger.setGeometry(24, 24, 100, 32);
  QPointer<AdPopover> popover = new AdPopover;
  popover->setSourceWidget(&trigger);
  popover->setText(QStringLiteral("Observed popup"));
  connect(popover, &AdPopover::popupOpening, &host, [&] { delete popover.data(); });
  host.show();
  popover->show();
  QVERIFY(popover.isNull());
  QTest::mouseClick(&trigger, Qt::LeftButton);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void QtToolPopupTest::popoverShowEventAllowsOwnerDeletion() {
  class DeletionFilter final : public QObject {
   public:
    std::function<void()> onShow;
    bool eventFilter(QObject* watched, QEvent* event) override {
      if (event->type() != QEvent::Show) {
        return false;
      }
      const QPointer<QObject> receiver(watched);
      onShow();
      return receiver.isNull();
    }
  };

  for (const auto layer :
       {AdPopover::PopupLayerMode::InWindow, AdPopover::PopupLayerMode::QtTool}) {
    QWidget host;
    host.resize(640, 360);
    QPushButton trigger(QStringLiteral("Open"), &host);
    trigger.setGeometry(24, 24, 100, 32);
    QPointer<AdPopover> popover = new AdPopover;
    popover->setSourceWidget(&trigger);
    popover->setPopupLayerMode(layer);
    popover->setText(QStringLiteral("Observed popup"));
    host.show();
    popover->preparePopup();
    QPointer<QWidget> surface = findSurface(QStringLiteral("adpopover-surface"));
    QVERIFY(surface);
    DeletionFilter observer;
    observer.onShow = [&]() { delete popover.data(); };
    surface->installEventFilter(&observer);
    popover->show();
    QVERIFY(popover.isNull());
    QVERIFY(surface);
    QVERIFY(!surface->isVisible());
    QTest::mouseClick(&trigger, Qt::LeftButton);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

void QtToolPopupTest::popoverPropertyObserversAllowOwnerDeletion() {
  const auto verify = [&](auto signal, const auto& change) {
    QWidget host;
    host.resize(640, 360);
    QPushButton trigger(QStringLiteral("Open"), &host);
    QPointer<AdPopover> popover = new AdPopover;
    popover->setSourceWidget(&trigger);
    popover->setText(QStringLiteral("Initial content"));
    int notifications = 0;
    connect(popover, signal, &host, [&]() {
      ++notifications;
      delete popover.data();
    });
    change(popover.data(), &host);
    QCOMPARE(notifications, 1);
    QVERIFY(popover.isNull());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  };

  verify(&AdPopover::placementChanged,
         [](auto* p, QWidget*) { p->setPlacement(AdPopover::Placement::Left); });
  verify(&AdPopover::triggersChanged,
         [](auto* p, QWidget*) { p->setTriggers(AdPopover::Trigger::Click); });
  verify(&AdPopover::retainNativeSurfaceOnHideChanged, [](auto* p, QWidget*) {
    p->setRetainNativeSurfaceOnHide(!p->retainNativeSurfaceOnHide());
  });
  verify(&AdPopover::visibilityPolicyChanged,
         [](auto* p, QWidget*) { p->setVisibilityPolicy(AdPopover::VisibilityPolicy::Manual); });
  verify(&AdPopover::popupLifetimeChanged,
         [](auto* p, QWidget*) { p->setPopupLifetime(AdPopover::PopupLifetime::RecreateOnOpen); });
  verify(&AdPopover::destroyOnHiddenChanged,
         [](auto* p, QWidget*) { p->setDestroyOnHidden(true); });
  verify(&AdPopover::popupLayerModeChanged,
         [](auto* p, QWidget*) { p->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool); });
  verify(&AdPopover::defaultVisibleChanged, [](auto* p, QWidget*) { p->setDefaultVisible(true); });
  verify(&AdPopover::autoAdjustOverflowChanged,
         [](auto* p, QWidget*) { p->setAutoAdjustOverflow(!p->autoAdjustOverflow()); });
  verify(&AdPopover::arrowVisibleChanged,
         [](auto* p, QWidget*) { p->setArrowVisible(!p->arrowVisible()); });
  verify(&AdPopover::arrowPointAtCenterChanged,
         [](auto* p, QWidget*) { p->setArrowPointAtCenter(!p->arrowPointAtCenter()); });
  verify(&AdPopover::hoverOpenDelayMsChanged,
         [](auto* p, QWidget*) { p->setHoverOpenDelayMs(p->hoverOpenDelayMs() + 1); });
  verify(&AdPopover::hoverCloseDelayMsChanged,
         [](auto* p, QWidget*) { p->setHoverCloseDelayMs(p->hoverCloseDelayMs() + 1); });
  verify(&AdPopover::enabledChanged, [](auto* p, QWidget*) { p->setEnabled(false); });
  verify(&AdPopover::titleChanged,
         [](auto* p, QWidget*) { p->setTitle(QStringLiteral("Changed title")); });
  verify(&AdPopover::textChanged,
         [](auto* p, QWidget*) { p->setText(QStringLiteral("Changed content")); });
  verify(&AdPopover::sourceWidgetChanged, [](auto* p, QWidget* host) { p->setSourceWidget(host); });
  verify(&AdPopover::anchorWidgetChanged, [](auto* p, QWidget* host) { p->setAnchorWidget(host); });
  verify(&AdPopover::titleWidgetChanged, [](auto* p, QWidget*) { p->setTitleWidget(new QWidget); });
  verify(&AdPopover::contentWidgetChanged,
         [](auto* p, QWidget*) { p->setContentWidget(new QWidget); });
  verify(&AdPopover::titleFontChanged, [](auto* p, QWidget*) {
    QFont font = p->titleFont();
    font.setBold(!font.bold());
    p->setTitleFont(font);
  });
  verify(&AdPopover::textFontChanged, [](auto* p, QWidget*) {
    QFont font = p->textFont();
    font.setBold(!font.bold());
    p->setTextFont(font);
  });
  verify(&AdPopover::backgroundColorChanged,
         [](auto* p, QWidget*) { p->setBackgroundColor(Qt::magenta); });
  verify(&AdPopover::borderColorChanged, [](auto* p, QWidget*) { p->setBorderColor(Qt::magenta); });
  verify(&AdPopover::titleColorChanged, [](auto* p, QWidget*) { p->setTitleColor(Qt::magenta); });
  verify(&AdPopover::textColorChanged, [](auto* p, QWidget*) { p->setTextColor(Qt::magenta); });
  verify(&AdPopover::contentMarginsChanged, [](auto* p, QWidget*) {
    p->setContentMargins(p->contentMargins() + QMargins(1, 1, 1, 1));
  });
  verify(&AdPopover::titleMinimumWidthChanged,
         [](auto* p, QWidget*) { p->setTitleMinimumWidth(p->titleMinimumWidth() + 1); });
  verify(&AdPopover::maximumWidthChanged,
         [](auto* p, QWidget*) { p->setMaximumWidth(p->maximumWidth() + 1); });
  verify(&AdPopover::zIndexChanged, [](auto* p, QWidget*) { p->setZIndex(p->zIndex() + 1); });
  verify(&AdPopover::cornerRadiusChanged,
         [](auto* p, QWidget*) { p->setCornerRadius(p->cornerRadius() + 1); });
  verify(&AdPopover::borderWidthChanged,
         [](auto* p, QWidget*) { p->setBorderWidth(p->borderWidth() + 1); });
  verify(&AdPopover::arrowSizeChanged,
         [](auto* p, QWidget*) { p->setArrowSize(p->arrowSize() + 1); });
  verify(&AdPopover::popupOffsetChanged,
         [](auto* p, QWidget*) { p->setPopupOffset(p->popupOffset() + 1); });
}

void QtToolPopupTest::recreateLifetimeStillDestroysPopupSurface() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPopupLifetime(AdPopover::PopupLifetime::RecreateOnOpen);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QPointer<QWidget> firstSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(firstSurface);
  QTRY_VERIFY(firstSurface->isVisible());

  popover.hide();
  QTRY_VERIFY(!popover.isVisible());
  QTRY_VERIFY(firstSurface.isNull());

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* secondSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(secondSurface);
}

void QtToolPopupTest::retainedPopupCachesFollowVisibilityAndStayComponentLocal() {
  QWidget host;
  host.resize(640, 360);
  adqt::widgets::detail::OverlayPopupSurface first(&host);
  adqt::widgets::detail::OverlayPopupSurface second(&host);
  first.resize(180, 96);
  second.resize(180, 96);
  first.move(24, 80);
  second.move(240, 80);

  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasPathCache(first));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasShadowCache(first));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  first.show();
  second.show();
  QTRY_VERIFY(first.isVisible() && second.isVisible());
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(second));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(second));

  first.hide();
  QTRY_VERIFY(!first.isVisible());
  QVERIFY(!first.containsInteractiveLocalPos(QPointF(40.0, 40.0)));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasPathCache(first));
  QVERIFY(!adqt::widgets::detail::OverlayPopupSurfaceTestAccess::hasShadowCache(first));
  QVERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(second));
  QVERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(second));

  first.show();
  QTRY_VERIFY(first.isVisible());
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::pathCacheValid(first));
  QTRY_VERIFY(adqt::widgets::detail::OverlayPopupSurfaceTestAccess::shadowCacheValid(first));
}

void QtToolPopupTest::popoverDestructionAllowsUiEvents() {
  for (const auto layer :
       {AdPopover::PopupLayerMode::InWindow, AdPopover::PopupLayerMode::QtTool}) {
    for (int state = 0; state < 3; ++state) {
      QWidget host;
      host.resize(640, 360);
      QPushButton trigger(QStringLiteral("Open"), &host);
      trigger.setGeometry(24, 24, 100, 32);
      auto* popover = new AdPopover;
      popover->setSourceWidget(&trigger);
      popover->setTriggers(AdPopover::Trigger::Click);
      popover->setPopupLayerMode(layer);
      popover->setText(QStringLiteral("Popup content"));
      host.show();
      if (state > 0) popover->show();
      if (state == 2) popover->hide();

      bool replacementShown = false;
      connect(popover, &QObject::destroyed, &host, [&]() {
        // Destruction observers may immediately update the UI. The popup's application
        // filter must no longer call its delegate after the derived destructor returns.
        QTest::mouseClick(&trigger, Qt::LeftButton);
        QWidget replacement;
        replacement.show();
        replacementShown = replacement.isVisible();
      });
      delete popover;
      QVERIFY(replacementShown);
      QVERIFY(!findSurface(QStringLiteral("adpopover-surface"), true));
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
  }
}

void QtToolPopupTest::tooltipDestructionAllowsUiEvents() {
  for (const auto layer :
       {AdTooltip::LayerMode::InWindow, AdTooltip::LayerMode::TopLevelTransient}) {
    for (int state = 0; state < 3; ++state) {
      QWidget host;
      host.resize(640, 360);
      QPushButton target(QStringLiteral("Target"), &host);
      target.setGeometry(24, 24, 100, 32);
      auto* tooltip = new AdTooltip;
      tooltip->setTargetWidget(&target);
      tooltip->setLayerMode(layer);
      tooltip->setTriggers(AdTooltip::Trigger::Click);
      tooltip->setText(QStringLiteral("Tooltip content"));
      host.show();
      if (state > 0) tooltip->show();
      if (state == 2) tooltip->hide();

      bool replacementShown = false;
      // Observe child lifetimes as UI integrations can do. The delegate is a child
      // of AdTooltip, and its own children must not observe input after it is torn down.
      for (QObject* child : tooltip->findChildren<QObject*>()) {
        connect(child, &QObject::destroyed, &host, [&]() {
          QTest::mouseClick(&target, Qt::LeftButton);
          QWidget replacement;
          replacement.show();
          replacementShown = replacement.isVisible();
        });
      }
      delete tooltip;
      QVERIFY(replacementShown);
      QVERIFY(!findSurface(QStringLiteral("adtooltip-surface"), true));
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
  }
}

void QtToolPopupTest::datePickerDestructionAllowsUiEvents() {
  for (const auto layer :
       {AdDatePicker::PopupLayerMode::InWindow, AdDatePicker::PopupLayerMode::QtTool}) {
    for (int state = 0; state < 3; ++state) {
      QWidget host;
      host.resize(760, 360);
      QPushButton trigger(QStringLiteral("Target"), &host);
      trigger.setGeometry(400, 24, 100, 32);
      host.show();
      const auto verifyDestruction = [&](auto* picker) {
        picker->setGeometry(24, 24, 320, 32);
        picker->setPopupLayerMode(layer);
        picker->show();
        if (state > 0) picker->showPopup();
        if (state == 2) picker->hidePopup();
        int notifications = 0;
        for (QObject* child : picker->template findChildren<QObject*>()) {
          connect(child, &QObject::destroyed, &host, [&]() {
            ++notifications;
            QTest::mouseClick(&trigger, Qt::LeftButton);
            QWidget replacement;
            replacement.show();
          });
        }
        delete picker;
        QVERIFY(notifications > 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      };
      verifyDestruction(new AdDatePicker(&host));
      verifyDestruction(new AdDateRangePicker(&host));
    }
  }
}

void QtToolPopupTest::datePickerPopupReplacementAllowsOwnerDeletion() {
  for (const auto layer :
       {AdDatePicker::PopupLayerMode::InWindow, AdDatePicker::PopupLayerMode::QtTool}) {
    for (int replacement = 0; replacement < 3; ++replacement) {
      QWidget host;
      host.resize(760, 360);
      host.show();
      const auto verify = [&](auto* picker, auto signal) {
        picker->setGeometry(24, 24, 320, 32);
        picker->setPopupLayerMode(layer);
        picker->show();
        picker->showPopup();
        QVERIFY(picker->popupVisible());
        QPointer<QObject> lifetime(picker);
        int notifications = 0;
        connect(picker, signal, &host, [&](bool visible) {
          if (!visible) {
            ++notifications;
            delete picker;
          }
        });
        if (replacement == 0) {
          picker->setPopupLayerMode(layer == AdDatePicker::PopupLayerMode::InWindow
                                        ? AdDatePicker::PopupLayerMode::QtTool
                                        : AdDatePicker::PopupLayerMode::InWindow);
        } else if (replacement == 1) {
          picker->clearPopupContentWrapperFactory();
        } else {
          picker->clearPanelComponentFactory();
        }
        QCOMPARE(notifications, 1);
        QVERIFY(lifetime.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      };
      verify(new AdDatePicker(&host), &AdDatePicker::popupVisibleChanged);
      verify(new AdDateRangePicker(&host), &AdDateRangePicker::popupVisibleChanged);
    }
  }
}

void QtToolPopupTest::retainedFactoryContentIsLazyAndOwnerBound() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  auto* popover = new AdPopover(trigger);
  popover->setSourceWidget(trigger);
  popover->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);

  int creationCount = 0;
  popover->setContentFactory([&creationCount]() {
    ++creationCount;
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });
  QCOMPARE(popover->contentWidget(), nullptr);
  QCOMPARE(creationCount, 0);

  bool contentExistedWhenShown = false;
  connect(popover, &AdPopover::visibleChanged, popover,
          [popover, &contentExistedWhenShown](bool value) {
            if (value) {
              contentExistedWhenShown = popover->contentWidget() != nullptr;
            }
          });
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover->show();
  QTRY_VERIFY(popover->isVisible());
  QCOMPARE(creationCount, 1);
  QVERIFY(contentExistedWhenShown);
  QPointer<QWidget> retainedContent = popover->contentWidget();
  QVERIFY(retainedContent);

  popover->hide();
  QTRY_VERIFY(!popover->isVisible());
  QCOMPARE(popover->contentWidget(), retainedContent.data());
  QVERIFY(retainedContent);

  popover->show();
  QTRY_VERIFY(popover->isVisible());
  QCOMPARE(creationCount, 1);
  QCOMPARE(popover->contentWidget(), retainedContent.data());

  delete popover;
  QTRY_VERIFY(retainedContent.isNull());
}

void QtToolPopupTest::recreateFactoryContentIsReleasedAfterHide() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);

  int creationCount = 0;
  popover.setContentFactory(
      [&creationCount]() {
        auto* content = new QWidget;
        content->setFixedSize(120, 48);
        content->setProperty("factoryGeneration", ++creationCount);
        auto* button = new QPushButton(QStringLiteral("Option"), content);
        button->setGeometry(8, 8, 80, 28);
        return content;
      },
      AdPopover::FactoryContentLifetime::RecreateOnOpen);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(creationCount, 1);
  QPointer<QWidget> firstContent = popover.contentWidget();
  QPointer<QPushButton> firstButton = firstContent->findChild<QPushButton*>();
  QVERIFY(firstContent && firstButton);

  popover.hide();
  QTRY_VERIFY(!popover.isVisible());
  QCOMPARE(popover.contentWidget(), nullptr);
  QTRY_VERIFY(firstContent.isNull());
  QTRY_VERIFY(firstButton.isNull());

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(creationCount, 2);
  QVERIFY(popover.contentWidget());
  QCOMPARE(popover.contentWidget()->property("factoryGeneration").toInt(), 2);
  QVERIFY(popover.contentWidget()->findChild<QPushButton*>());
}

void QtToolPopupTest::directContentRemainsCompatibleWithFactoryApi() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);
  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setContentFactory([]() {
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });
  popover.setContentWidget(nullptr);
  QVERIFY(!popover.contentFactory());
  popover.setContentFactory([]() {
    auto* content = new QWidget;
    content->setFixedSize(120, 48);
    return content;
  });

  auto* directContent = new QWidget;
  directContent->setFixedSize(140, 52);
  popover.setContentWidget(directContent);
  QVERIFY(!popover.contentFactory());
  QCOMPARE(popover.contentWidget(), directContent);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  for (int cycle = 0; cycle < 2; ++cycle) {
    popover.show();
    QTRY_VERIFY(popover.isVisible());
    QCOMPARE(popover.contentWidget(), directContent);
    popover.hide();
    QTRY_VERIFY(!popover.isVisible());
    QCOMPARE(popover.contentWidget(), directContent);
  }
}

void QtToolPopupTest::colorPickerPrewarmCanBeDisabled() {
  const auto pickerPanelCount = []() {
    const QWidgetList widgets = QApplication::allWidgets();
    return static_cast<int>(std::count_if(widgets.cbegin(), widgets.cend(), [](QWidget* widget) {
      return widget && widget->objectName() == QStringLiteral("ad-color-picker-picker-panel");
    }));
  };
  const int initialPickerPanelCount = pickerPanelCount();
  QWidget host;
  host.resize(640, 360);
  auto* defaultPicker = new AdColorPicker(&host);
  defaultPicker->setGeometry(24, 24, 120, 32);
  auto* explicitPicker = new AdColorPicker(&host);
  explicitPicker->setGeometry(24, 80, 120, 32);
  explicitPicker->setPopupPrewarmEnabled(false);
  auto* hoverPicker = new AdColorPicker(&host);
  hoverPicker->setGeometry(24, 136, 120, 32);
  hoverPicker->setPopupPrewarmEnabled(false);
  hoverPicker->setTrigger(AdColorPicker::Trigger::Hover);

  QCOMPARE(defaultPicker->popupPrewarmEnabled(), true);
  QCOMPARE(explicitPicker->popupPrewarmEnabled(), false);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  QTRY_COMPARE(pickerPanelCount(), initialPickerPanelCount + 1);
  QTest::qWait(100);
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 1);

  explicitPicker->setPopupVisible(true);
  QTRY_VERIFY(explicitPicker->popupVisible());
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 2);
  explicitPicker->setPopupVisible(false);
  QTRY_VERIFY(!explicitPicker->popupVisible());

  QWidget* hoverTrigger =
      hoverPicker->findChild<QWidget*>(QStringLiteral("ad-color-picker-trigger-frame"));
  QVERIFY(hoverTrigger);
  QTest::mouseMove(&host, QPoint(400, 280));
  QTest::mouseMove(hoverTrigger, hoverTrigger->rect().center());
  QTRY_VERIFY(hoverPicker->popupVisible());
  QCOMPARE(pickerPanelCount(), initialPickerPanelCount + 3);
}

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QStringList arguments = application.arguments();
  if (arguments.removeAll(QStringLiteral("--date-picker-popup-lifetime-only")) > 0) {
    arguments.append(QStringLiteral("datePickerSurvivesPopupContainerDestructionOrder"));
    arguments.append(QStringLiteral("dateRangePickerSurvivesPopupContainerDestructionOrder"));
  }
  QtToolPopupTest test;
  return QTest::qExec(&test, arguments);
}

#include "qt_tool_popup_tests.moc"

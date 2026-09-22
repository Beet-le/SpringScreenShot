#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H

#include <QFrame>

class QAbstractButton;
class QEvent;
class QMouseEvent;
class QWidget;
namespace snow_shot::presentation::styles {
struct ThemeAliasMetricToken;
struct ThemeColorScheme;
} // namespace snow_shot::presentation::styles

class TitleBarWidget : public QFrame {
    Q_OBJECT

  public:
    explicit TitleBarWidget(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                            QWidget* parent = nullptr);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void setMaximized(bool maximized);

    QAbstractButton* minimizeButton() const;
    QAbstractButton* maximizeButton() const;
    QAbstractButton* closeButton() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    void retranslateUi();

    QAbstractButton* m_minimizeButton = nullptr;
    QAbstractButton* m_maximizeButton = nullptr;
    QAbstractButton* m_closeButton = nullptr;
    bool m_maximized = false;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H

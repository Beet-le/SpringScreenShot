#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
bool withinSnapTolerance(const QRectF& selection, const QRectF& target) {
    constexpr qreal tolerance = ScreenshotSmartSelectionTransition::kSnapTolerance;
    return std::abs(selection.left() - target.left()) <= tolerance &&
           std::abs(selection.top() - target.top()) <= tolerance &&
           std::abs(selection.right() - target.right()) <= tolerance &&
           std::abs(selection.bottom() - target.bottom()) <= tolerance;
}
} // namespace

ScreenshotSmartSelectionTransition::ScreenshotSmartSelectionTransition(UpdateCallback update)
    : m_update(std::move(update)) {}

void ScreenshotSmartSelectionTransition::setAnimation(
    snow_shot::SelectionTransitionAnimation animation, qint64 nowMs) {
    animation = snow_shot::normalizedSelectionTransitionAnimation(animation);
    if (m_animation == animation) {
        return;
    }
    static_cast<void>(advance(nowMs));
    m_lastAdvancedAtMs = std::max(nowMs, m_lastAdvancedAtMs);
    m_animation = animation;
    if (m_animation == snow_shot::SelectionTransitionAnimation::Disabled && m_running) {
        static_cast<void>(presentDirectly(m_targetSelection));
    }
}

snow_shot::SelectionTransitionAnimation ScreenshotSmartSelectionTransition::animation() const {
    return m_animation;
}

bool ScreenshotSmartSelectionTransition::update(const QRectF& selection, bool smartFraming,
                                                qint64 nowMs) {
    const bool hasSelection = selection.isValid() && !selection.isEmpty();
    if (!smartFraming || !hasSelection) {
        m_hasPresentedSmartSelection = false;
        return presentDirectly(selection);
    }

    if (m_animation == snow_shot::SelectionTransitionAnimation::Disabled ||
        !m_hasPresentedSmartSelection) {
        m_hasPresentedSmartSelection = true;
        return presentDirectly(selection);
    }

    if (selection == m_targetSelection) {
        return false;
    }

    // Consume elapsed time against the preceding target before following the new one.
    // Presentation owns the clock and decides when frames are committed.
    static_cast<void>(advance(nowMs));
    m_lastAdvancedAtMs = std::max(nowMs, m_lastAdvancedAtMs);
    m_targetSelection = selection;
    if (withinSnapTolerance(m_displayedSelection, m_targetSelection)) {
        static_cast<void>(presentDirectly(selection));
    } else {
        m_running = true;
    }
    return true;
}

bool ScreenshotSmartSelectionTransition::advance(qint64 nowMs) {
    if (!m_running || nowMs <= m_lastAdvancedAtMs) {
        return false;
    }
    const qreal elapsedSeconds = static_cast<qreal>(nowMs - m_lastAdvancedAtMs) / 1000.0;
    m_lastAdvancedAtMs = nowMs;
    const qreal amount = -std::expm1(-static_cast<int>(m_animation) * elapsedSeconds);
    const auto follow = [amount](qreal current, qreal target) {
        return current + (target - current) * amount;
    };
    QRectF selection(QPointF(follow(m_displayedSelection.left(), m_targetSelection.left()),
                             follow(m_displayedSelection.top(), m_targetSelection.top())),
                     QPointF(follow(m_displayedSelection.right(), m_targetSelection.right()),
                             follow(m_displayedSelection.bottom(), m_targetSelection.bottom())));
    if (withinSnapTolerance(selection, m_targetSelection)) {
        m_running = false;
        selection = m_targetSelection;
    }
    if (selection == m_displayedSelection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

bool ScreenshotSmartSelectionTransition::isRunning() const {
    return m_running;
}

QRectF ScreenshotSmartSelectionTransition::displayedSelection() const {
    return m_displayedSelection;
}

bool ScreenshotSmartSelectionTransition::presentDirectly(const QRectF& selection) {
    m_running = false;
    m_targetSelection = selection;
    if (m_displayedSelection == selection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

void ScreenshotSmartSelectionTransition::notifyUpdate() {
    if (m_update) {
        m_update(m_displayedSelection);
    }
}

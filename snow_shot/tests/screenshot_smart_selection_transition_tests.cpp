#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using Animation = snow_shot::SelectionTransitionAnimation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearRect(const QRectF& actual, const QRectF& expected) {
    constexpr qreal tolerance = 0.000001;
    return std::abs(actual.x() - expected.x()) < tolerance &&
           std::abs(actual.y() - expected.y()) < tolerance &&
           std::abs(actual.width() - expected.width()) < tolerance &&
           std::abs(actual.height() - expected.height()) < tolerance;
}

QRectF followed(const QRectF& start, const QRectF& target, qreal elapsedMs, qreal rate = 32.0) {
    const qreal eased = 1.0 - std::exp(-rate * elapsedMs / 1000.0);
    return QRectF(start.x() + (target.x() - start.x()) * eased,
                  start.y() + (target.y() - start.y()) * eased,
                  start.width() + (target.width() - start.width()) * eased,
                  start.height() + (target.height() - start.height()) * eased);
}

void firstSmartSelectionIsPresentedDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);

    require(transition.animation() == Animation::Soft, "selection animation must default to Soft");
    require(transition.update(first, true, 1000), "first smart selection must change presentation");
    require(updateCount == 1, "first smart selection must update immediately");
    require(presented == first, "first smart selection must not be interpolated");
    require(!transition.isRunning(), "first smart selection must not animate");
    require(!transition.advance(2000), "direct selection must not produce later frames");
}

void framesUseElapsedTimeForExponentialFollowing() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    require(transition.update(second, true, 1100), "new smart target must start a transition");
    require(transition.isRunning(), "subsequent smart selection must animate");
    require(presented == first, "transition must begin at the displayed rectangle");
    require(!transition.advance(1100), "transition start must not repeat the previous frame");

    require(transition.advance(1108), "an eight-millisecond frame must advance presentation");
    require(nearRect(presented, followed(first, second, 8.0)),
            "Soft must follow the target at 32 per second using elapsed time");
    require(transition.advance(1116), "high-refresh frames must not wait for a second timer");
    require(nearRect(presented, followed(first, second, 16.0)),
            "high-refresh frames must preserve the chase rate");
    require(presented.left() < first.left() + (second.left() - first.left()) * 0.5,
            "the first sixteen milliseconds must leave most of the motion for users to track");
    const int updatesAfterFrame = updateCount;
    require(!transition.advance(1116), "repeated frame timestamps must not emit an update");
    require(!transition.advance(1110), "older frame timestamps must not reverse the transition");
    require(updateCount == updatesAfterFrame, "repeated or older frames must leave callbacks idle");

    require(transition.advance(1148), "a delayed frame must use all elapsed time");
    require(nearRect(presented, followed(first, second, 48.0)),
            "a delayed frame must catch up without assuming a fixed tick interval");
    require(transition.isRunning(),
            "following must continue while an edge is outside snap tolerance");
    require(transition.advance(1200),
            "following must continue through the first hundred milliseconds");
    require(
        nearRect(presented, followed(first, second, 100.0)) && transition.isRunning(),
        "larger selection changes must retain a visible transition after one hundred milliseconds");
    require(transition.advance(1380), "a converged frame must reach the target exactly");
    require(!transition.isRunning(),
            "following must stop once every edge is within snap tolerance");
    require(presented == second, "smart selection transition must end at its exact target");
    require(!transition.advance(100000), "completed transition must not emit extra frames");
}

void frameCadenceDoesNotChangeTheTrajectory() {
    ScreenshotSmartSelectionTransition regular({});
    ScreenshotSmartSelectionTransition irregular({});
    ScreenshotSmartSelectionTransition delayed({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    for (auto* transition : {&regular, &irregular, &delayed}) {
        static_cast<void>(transition->update(first, true, 1000));
        static_cast<void>(transition->update(target, true, 1100));
    }
    for (qint64 elapsedMs = 8; elapsedMs <= 64; elapsedMs += 8)
        static_cast<void>(regular.advance(1100 + elapsedMs));
    for (const qint64 elapsedMs : {5, 17, 41, 64})
        static_cast<void>(irregular.advance(1100 + elapsedMs));
    static_cast<void>(delayed.advance(1164));

    const QRectF expected = followed(first, target, 64.0);
    require(nearRect(regular.displayedSelection(), expected) &&
                nearRect(irregular.displayedSelection(), expected) &&
                nearRect(delayed.displayedSelection(), expected),
            "regular, irregular and delayed frames must reach the same geometry at the same time");
}

void retargetingSamplesTheCurrentTrajectory() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);
    const QRectF third(30.0, 200.0, 100.0, 80.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.update(third, true, 1150));
    const QRectF retargetStart = followed(first, second, 50.0);
    require(nearRect(presented, retargetStart),
            "retargeting must sample the preceding trajectory at the supplied timestamp");

    static_cast<void>(transition.advance(1200));
    require(nearRect(presented, followed(retargetStart, third, 50.0)),
            "retargeted animation must follow from the current rectangle at the same chase rate");
    static_cast<void>(transition.advance(100000));
    require(presented == third, "a delayed frame must finish at the latest target");
    require(!transition.isRunning(), "a delayed final frame must stop the transition");
}

void retargetingDoesNotRewindTheClock() {
    ScreenshotSmartSelectionTransition transition({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);
    const QRectF third(30.0, 200.0, 100.0, 80.0);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.advance(1116));
    const QRectF current = transition.displayedSelection();
    static_cast<void>(transition.update(third, true, 1108));
    require(transition.displayedSelection() == current,
            "retargeting with an old timestamp must not reverse the displayed trajectory");
    static_cast<void>(transition.advance(1124));
    require(nearRect(transition.displayedSelection(), followed(current, third, 8.0)),
            "retargeting with an old timestamp must not count already consumed time again");
}

void subpixelTargetChangesSnapImmediately() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target = first.translated(0.25, -0.25);
    static_cast<void>(transition.update(first, true, 1000));
    require(transition.update(target, true, 1100), "a nearby target must change presentation");
    require(transition.displayedSelection() == target && !transition.isRunning(),
            "a target within 0.4 pixels on every edge must snap without scheduling animation");
    require(!transition.advance(1116) && updateCount == 2,
            "snapped target changes must notify exactly once and leave no animation frames");
}

void snappingWaitsForEveryEdge() {
    const QRectF first(0.0, 0.0, 100.0, 100.0);
    for (const QRectF& target : {QRectF(0.25, 0.0, 100.25, 100.0), QRectF(0.0, 0.25, 100.0, 100.25),
                                 QRectF(0.5, 0.0, 99.75, 100.0), QRectF(0.0, 0.5, 100.0, 99.75)}) {
        ScreenshotSmartSelectionTransition transition({});
        static_cast<void>(transition.update(first, true, 1000));
        static_cast<void>(transition.update(target, true, 1100));
        require(transition.isRunning(), "any edge outside 0.4 pixels must keep following active");
        static_cast<void>(transition.advance(1101));
        require(transition.isRunning() &&
                    nearRect(transition.displayedSelection(), followed(first, target, 1.0)),
                "snapping must check all four edges rather than position and size separately");
        static_cast<void>(transition.advance(1112));
        require(!transition.isRunning() && transition.displayedSelection() == target,
                "converged edges must snap to the exact target and stop animation");
    }
}

void disabledTransitionPresentsSmartSelectionsDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    transition.setAnimation(Animation::Disabled, 1000);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1008));

    require(transition.animation() == Animation::Disabled,
            "disabled smart selection transition must remain disabled");
    require(!transition.isRunning(), "disabled smart selection transition must not animate");
    require(presented == second, "disabled transition must present the latest target directly");
    require(!transition.update(second, true, 1016),
            "unchanged disabled selection must not report a presentation change");
    require(updateCount == 2, "disabled transition must emit each changed selection once");
}

void disablingRunningTransitionPresentsItsTargetDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.advance(1116));
    transition.setAnimation(Animation::Disabled, 1116);
    const int updateCountAfterDisable = updateCount;

    require(transition.animation() == Animation::Disabled,
            "running smart selection transition must become disabled");
    require(!transition.isRunning(), "disabling a running transition must stop its animation");
    require(presented == second,
            "disabling a running transition must immediately reach its target");
    require(!transition.advance(1201), "disabled transition must not produce stale frames");
    require(updateCount == updateCountAfterDisable,
            "disabled transition must not emit stale animation callbacks");
}

void leavingSmartFramingAndClearingResetTheFirstResultRule() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    const QRectF manual(30.0, 40.0, 100.0, 80.0);
    const QRectF nextFirst(400.0, 300.0, 250.0, 180.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(target, true, 1100));
    static_cast<void>(transition.update(manual, false, 1116));
    require(presented == manual, "leaving smart framing must present manual geometry immediately");
    require(!transition.advance(1201), "leaving smart framing must cancel all animation frames");
    static_cast<void>(transition.update(nextFirst, true, 1300));
    require(!transition.isRunning(), "first result after re-entry must not animate");
    require(presented == nextFirst, "first result after re-entry must be direct");

    static_cast<void>(transition.update(target, true, 1400));
    static_cast<void>(transition.update({}, true, 1416));
    require(presented.isEmpty(), "clearing must present an empty selection immediately");
    require(!transition.advance(1501), "clearing must cancel all animation frames");
    static_cast<void>(transition.update(first, true, 1600));
    require(!transition.isRunning(), "first result after clearing must not animate");
    require(presented == first, "first result after clearing must be direct");
}

void unchangedTargetsAndDirectGeometryDoNotEmitUpdates() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    require(transition.update(first, true, 1000), "first smart selection must report a change");
    require(!transition.update(first, true, 1050), "unchanged smart target must not restart");
    require(updateCount == 1, "unchanged smart target must not emit an update");
    static_cast<void>(transition.update(second, true, 1100));
    require(!transition.update(second, true, 1150), "unchanged running target must not restart");
    require(updateCount == 1,
            "target delivery must not advance frames independently of presentation");
    require(transition.advance(1150), "the presentation frame must still advance a running target");
    require(!transition.update(transition.displayedSelection(), false, 1150),
            "unchanged manual geometry must not emit a redundant update");
    require(!transition.isRunning(), "manual geometry must stop a running transition");
    require(!transition.update(transition.displayedSelection(), false, 1200),
            "repeated manual geometry must remain unchanged");
    require(updateCount == 2, "direct geometry repeats must leave presentation callbacks idle");
}

void fastAndSoftUseTheirSelectedRates() {
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    for (const auto animation : {Animation::Fast, Animation::Soft}) {
        ScreenshotSmartSelectionTransition transition({});
        transition.setAnimation(animation, 1000);
        static_cast<void>(transition.update(first, true, 1000));
        static_cast<void>(transition.update(target, true, 1100));
        static_cast<void>(transition.advance(1116));
        const qreal rate = animation == Animation::Fast ? 72.0 : 32.0;
        require(nearRect(transition.displayedSelection(), followed(first, target, 16.0, rate)),
                "Fast and Soft must follow at 72 and 32 per second respectively");
    }
}

void changingAnimationSamplesThePreviousRateBeforeSwitching() {
    ScreenshotSmartSelectionTransition transition({});
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(target, true, 1100));
    transition.setAnimation(Animation::Fast, 1124);
    const QRectF current = followed(first, target, 24.0);
    require(nearRect(transition.displayedSelection(), current),
            "changing speed must consume elapsed time at the previous rate");
    static_cast<void>(transition.advance(1132));
    require(nearRect(transition.displayedSelection(), followed(current, target, 8.0, 72.0)),
            "the selected rate must apply only to time after the speed change");
    transition.setAnimation(Animation::Fast, 1140);
    static_cast<void>(transition.advance(1148));
    require(nearRect(transition.displayedSelection(), followed(current, target, 24.0, 72.0)),
            "reapplying the same speed must not reset elapsed time");

    transition.setAnimation(Animation::Disabled, 1148);
    transition.setAnimation(Animation::Soft, 1200);
    static_cast<void>(transition.update(first, true, 1200));
    static_cast<void>(transition.advance(1216));
    require(nearRect(transition.displayedSelection(), followed(target, first, 16.0)),
            "reenabling animation must follow from the last directly presented target");
    transition.setAnimation(Animation::Fast, 1216);
    transition.setAnimation(static_cast<Animation>(-1), 1216);
    require(transition.animation() == Animation::Soft,
            "invalid animation modes must fall back to Soft");
}
} // namespace

int main() {
    firstSmartSelectionIsPresentedDirectly();
    framesUseElapsedTimeForExponentialFollowing();
    frameCadenceDoesNotChangeTheTrajectory();
    retargetingSamplesTheCurrentTrajectory();
    retargetingDoesNotRewindTheClock();
    subpixelTargetChangesSnapImmediately();
    snappingWaitsForEveryEdge();
    disabledTransitionPresentsSmartSelectionsDirectly();
    disablingRunningTransitionPresentsItsTargetDirectly();
    leavingSmartFramingAndClearingResetTheFirstResultRule();
    unchangedTargetsAndDirectGeometryDoNotEmitUpdates();
    fastAndSoftUseTheirSelectedRates();
    changingAnimationSamplesThePreviousRateBeforeSwitching();
    return 0;
}

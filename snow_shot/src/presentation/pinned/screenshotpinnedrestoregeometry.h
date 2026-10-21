#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

#include "pinnedplacementgeometry.h"

#include <QList>

namespace screenshot_pinned_restore_geometry {

struct PlacementState {
    snow_shot::storage::PinnedWindowPlacement placement;
    // Invalid when the window was not in thumbnail mode.
    snow_shot::storage::PinnedWindowPlacement preThumbnailPlacement;
    // Invalid when no hide-to-top handle was saved.
    snow_shot::storage::PinnedWindowPlacement hideToTopPlacement;
};

// Re-anchor every saved placement onto the target display, retaining its local
// position, window extent and geometry units whenever it intersects any current
// usable display area. Only wholly unreachable placements need recovery.
// Reconcile all variants together so callers derive native rectangles from the
// same placements used for presentation and subsequent persistence.
[[nodiscard]] PlacementState
reconcileSavedState(const PlacementState& saved,
                    const snow_shot::presentation::PinnedDisplayGeometry& targetScreen,
                    const QList<snow_shot::presentation::PinnedDisplayGeometry>& screens);

} // namespace screenshot_pinned_restore_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESTOREGEOMETRY_H

#include "screenshotpinnedrestoregeometry.h"

#include <algorithm>

namespace screenshot_pinned_restore_geometry {
namespace {

snow_shot::storage::PinnedWindowPlacement
restorePlacement(snow_shot::storage::PinnedWindowPlacement placement,
                 const snow_shot::presentation::PinnedDisplayGeometry& targetScreen,
                 const QList<snow_shot::presentation::PinnedDisplayGeometry>& screens) {
    if (!placement.isValid()) {
        return placement;
    }
    placement.displayName = targetScreen.name;
    placement.displaySerial = targetScreen.serial;
    const QRectF geometry = snow_shot::presentation::pinnedDesktopRect(placement, targetScreen);
    const bool visible =
        std::any_of(screens.begin(), screens.end(), [&geometry](const auto& screen) {
            return screen.usableBounds.intersects(geometry);
        });
    return visible ? placement
                   : snow_shot::presentation::recoverPinnedPlacement(placement, targetScreen);
}

} // namespace

PlacementState
reconcileSavedState(const PlacementState& saved,
                    const snow_shot::presentation::PinnedDisplayGeometry& targetScreen,
                    const QList<snow_shot::presentation::PinnedDisplayGeometry>& screens) {
    return {restorePlacement(saved.placement, targetScreen, screens),
            restorePlacement(saved.preThumbnailPlacement, targetScreen, screens),
            restorePlacement(saved.hideToTopPlacement, targetScreen, screens)};
}

} // namespace screenshot_pinned_restore_geometry

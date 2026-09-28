#include "snow_shot/app/launchcommands.h"

#include <QString>

namespace snow_shot::app {
LaunchAction launchActionForColdStart(const QStringList& arguments) {
    if (arguments.contains(QStringLiteral("--autostart"))) {
        return LaunchAction::None;
    }
    if (arguments.contains(QStringLiteral("--open-draw"))) {
        return LaunchAction::Screenshot;
    }
    if (arguments.contains(QStringLiteral("--show-main-window"))) {
        return LaunchAction::ShowMainWindow;
    }
    return LaunchAction::None;
}

LaunchAction launchActionForForwardedRequest(const QStringList& arguments) {
    if (arguments.contains(QStringLiteral("--autostart"))) {
        return LaunchAction::None;
    }
    if (arguments.contains(QStringLiteral("--open-draw"))) {
        return LaunchAction::Screenshot;
    }
    return LaunchAction::ShowMainWindow;
}
} // namespace snow_shot::app

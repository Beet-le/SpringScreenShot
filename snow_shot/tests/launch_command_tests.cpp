#include "snow_shot/app/launchcommands.h"

#include <QString>
#include <QStringList>

#include <cstdlib>
#include <iostream>

namespace {
using snow_shot::app::LaunchAction;
using snow_shot::app::launchActionForColdStart;
using snow_shot::app::launchActionForForwardedRequest;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QStringList arguments(const QString& second = QString()) {
    QStringList result{QStringLiteral("snow-shot")};
    if (!second.isEmpty()) {
        result.push_back(second);
    }
    return result;
}

void coldStartRules() {
    require(launchActionForColdStart(arguments()) == LaunchAction::None,
            "cold start without arguments must stay silent");
    require(launchActionForColdStart(arguments(QStringLiteral("--autostart"))) ==
                LaunchAction::None,
            "autostart cold start must stay silent");
    require(launchActionForColdStart(arguments(QStringLiteral("--show-main-window"))) ==
                LaunchAction::ShowMainWindow,
            "show-main-window cold start must activate the main window");
    require(launchActionForColdStart(arguments(QStringLiteral("--open-draw"))) ==
                LaunchAction::Screenshot,
            "open-draw cold start must trigger a capture");
    QStringList combined{QStringLiteral("snow-shot"), QStringLiteral("--show-main-window"),
                         QStringLiteral("--open-draw")};
    require(launchActionForColdStart(combined) == LaunchAction::Screenshot,
            "open-draw must win over show-main-window on cold start");
    QStringList autostartCombined{QStringLiteral("snow-shot"), QStringLiteral("--autostart"),
                                  QStringLiteral("--open-draw")};
    require(launchActionForColdStart(autostartCombined) == LaunchAction::None,
            "autostart must never trigger a screenshot on cold start");
}

void forwardedRequestRules() {
    require(launchActionForForwardedRequest(arguments()) == LaunchAction::ShowMainWindow,
            "plain forwarded request must activate the main window");
    require(launchActionForForwardedRequest(arguments(QStringLiteral("--autostart"))) ==
                LaunchAction::None,
            "forwarded autostart request must stay silent");
    require(launchActionForForwardedRequest(arguments(QStringLiteral("--open-draw"))) ==
                LaunchAction::Screenshot,
            "forwarded open-draw request must trigger a capture");
    QStringList autostartCombined{QStringLiteral("snow-shot"), QStringLiteral("--autostart"),
                                  QStringLiteral("--open-draw")};
    require(launchActionForForwardedRequest(autostartCombined) == LaunchAction::None,
            "autostart must win over open-draw in forwarded requests");
    QStringList unrecognised{QStringLiteral("snow-shot"), QStringLiteral("capture.png")};
    require(launchActionForForwardedRequest(unrecognised) == LaunchAction::ShowMainWindow,
            "unrecognised forwarded arguments must keep the activation default");
}
} // namespace

int main() {
    coldStartRules();
    forwardedRequestRules();
    return 0;
}

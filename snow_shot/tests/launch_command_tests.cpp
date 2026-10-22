#include "snow_shot/app/launchcommands.h"

#include <QFile>
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

// The launch wiring lives in main(), which no unit test can execute. Assert it on the
// startup source so a conflict resolution cannot silently drop the --open-draw fast path.
void startupWiringKeepsOpenDrawFastPath() {
#ifdef SNOW_SHOT_LAUNCH_MAIN_SOURCE
    QFile mainSource(QStringLiteral(SNOW_SHOT_LAUNCH_MAIN_SOURCE));
    require(mainSource.open(QIODevice::ReadOnly),
            "the application startup source must be readable for the wiring check");
    const QString source = QString::fromUtf8(mainSource.readAll());
    const qsizetype earlyForward = source.indexOf(QStringLiteral("tryForwardToRunningInstance"));
    const qsizetype applicationCreation =
        source.indexOf(QStringLiteral("QApplication app(argc, argv)"));
    require(earlyForward >= 0, "--open-draw must use the early single-instance forward fast path");
    require(applicationCreation >= 0 && earlyForward < applicationCreation,
            "the early forward must run before the application object is constructed");
    require(source.contains(QStringLiteral("launchActionForColdStart")),
            "cold start must dispatch the parsed launch action");
    require(source.contains(QStringLiteral("requestScreenshot")),
            "cold start --open-draw must request a screenshot");
#endif
}
} // namespace

int main() {
    coldStartRules();
    forwardedRequestRules();
    startupWiringKeepsOpenDrawFastPath();
    return 0;
}

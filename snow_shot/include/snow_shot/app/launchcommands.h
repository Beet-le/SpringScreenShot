#ifndef SNOW_SHOT_APP_LAUNCHCOMMANDS_H
#define SNOW_SHOT_APP_LAUNCHCOMMANDS_H

#include <QStringList>

namespace snow_shot::app {
// Launch arguments that ask the running (or newly started) instance to perform
// a user-facing action instead of the default activation behaviour.
enum class LaunchAction { None, ShowMainWindow, Screenshot };

// Cold start: --autostart wins over everything; --open-draw starts a capture;
// --show-main-window activates the main window; anything else stays hidden.
[[nodiscard]] LaunchAction launchActionForColdStart(const QStringList& arguments);

// Forwarded second-process request: --autostart is ignored, --open-draw starts
// a capture, and any other request activates the main window.
[[nodiscard]] LaunchAction launchActionForForwardedRequest(const QStringList& arguments);
} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_LAUNCHCOMMANDS_H

#ifndef SNOW_SHOT_APP_SHELLIMAGELAUNCH_H
#define SNOW_SHOT_APP_SHELLIMAGELAUNCH_H

#include <QDir>
#include <QStringList>

namespace snow_shot::app {

struct ShellImageLaunchRequest {
    bool requested = false;
    bool valid = true;
    QStringList paths;
};

// Explorer supplies a separate typed operand for each shell invocation. Repeated pairs also
// preserve multiple requests across an updater relaunch without parsing a shell command string.
[[nodiscard]] inline ShellImageLaunchRequest
parseShellImageLaunchRequest(const QStringList& arguments) {
    ShellImageLaunchRequest request;
    for (qsizetype index = 0; index < arguments.size(); ++index) {
        if (arguments.at(index) != QStringLiteral("--edit-with-snow-shot"))
            continue;
        request.requested = true;
        if (++index >= arguments.size()) {
            request.valid = false;
            break;
        }
        const QString& path = arguments.at(index);
        bool valid = !path.isEmpty() && !path.contains(u'"') && QDir::isAbsolutePath(path);
#ifdef Q_OS_WIN
        const QString normalized = QDir::fromNativeSeparators(path);
        const bool driveAbsolute = normalized.size() >= 3 && normalized.at(0).toUpper() >= u'A' &&
                                   normalized.at(0).toUpper() <= u'Z' && normalized.at(1) == u':' &&
                                   normalized.at(2) == u'/';
        const bool uncAbsolute = normalized.startsWith(QStringLiteral("//")) &&
                                 normalized.split(u'/', Qt::SkipEmptyParts).size() >= 2;
        valid = valid && (driveAbsolute || uncAbsolute);
#endif
        for (const QChar character : path)
            valid = valid && character.category() != QChar::Other_Control;
        if (!valid) {
            request.valid = false;
            break;
        }
        request.paths.append(path);
    }
    if (!request.valid)
        request.paths.clear();
    return request;
}

[[nodiscard]] inline QStringList shellImageLaunchArguments(const QStringList& paths) {
    QStringList arguments;
    for (const QString& path : paths) {
        arguments.append(QStringLiteral("--edit-with-snow-shot"));
        arguments.append(path);
    }
    return arguments;
}

} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_SHELLIMAGELAUNCH_H

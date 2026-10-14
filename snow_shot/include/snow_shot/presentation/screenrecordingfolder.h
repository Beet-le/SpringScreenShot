#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H

#include <QStringList>

namespace snow_shot::presentation::recording {
[[nodiscard]] QStringList screenRecordingDirectories();
[[nodiscard]] QString screenRecordingDirectory();
// The caller snapshots and expands the filename format before any worker handoff.
[[nodiscard]] QString screenRecordingOutputPath(const QStringList& directories,
                                                const QString& baseName, const QString& extension);
[[nodiscard]] bool openScreenRecordingFolder();
[[nodiscard]] bool revealScreenRecordingFile(const QString& path);
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H

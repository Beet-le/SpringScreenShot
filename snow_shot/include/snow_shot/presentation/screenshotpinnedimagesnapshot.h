// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDIMAGESNAPSHOT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDIMAGESNAPSHOT_H

#include "snow_shot/presentation/screenshotexportartifact.h"

// Owned image-export state captured on the GUI thread; rendering never reads a live pin.
struct ScreenshotPinnedImageSnapshot final {
    ScreenshotExportSource source;
    QSize outputSize;
    QString label;
    QString error = {};
};

#endif

// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_STORAGE_EXPORTFILENAME_H
#define SNOW_SHOT_STORAGE_EXPORTFILENAME_H

#include <QString>
#include <QStringView>

namespace snow_shot::storage {
enum class ExportFileNameControls { C0, C0AndC1 };

struct ExportFileNamePolicy {
    ExportFileNameControls controls = ExportFileNameControls::C0;
    // Zero preserves the complete stem. Positive limits never split a surrogate pair.
    qsizetype maximumUtf8Bytes = 0;
};

// The caller owns path/extension interpretation and the feature's fallback stem.
QString sanitizeExportFileName(QString name, QStringView fallback,
                               ExportFileNamePolicy policy = {});
} // namespace snow_shot::storage

#endif

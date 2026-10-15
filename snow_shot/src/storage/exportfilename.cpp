// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/storage/exportfilename.h"

namespace snow_shot::storage {
QString sanitizeExportFileName(QString name, QStringView fallback, ExportFileNamePolicy policy) {
    constexpr QStringView forbidden(u"<>:\"/\\|?*");
    for (QChar& character : name) {
        const ushort value = character.unicode();
        if (value < 0x20 ||
            (policy.controls == ExportFileNameControls::C0AndC1 && value >= 0x7f &&
             value <= 0x9f) ||
            forbidden.contains(character)) {
            character = u'_';
        }
    }
    name = name.trimmed();
    if (policy.maximumUtf8Bytes > 0) {
        if (name.size() > policy.maximumUtf8Bytes) {
            name.truncate(policy.maximumUtf8Bytes);
            if (name.back().isHighSurrogate())
                name.chop(1);
        }
        while (name.toUtf8().size() > policy.maximumUtf8Bytes) {
            name.chop(1);
            if (!name.isEmpty() && name.back().isHighSurrogate())
                name.chop(1);
        }
    }
    while (name.endsWith(u'.') || name.endsWith(u' '))
        name.chop(1);
    return name.isEmpty() ? fallback.toString() : name;
}
} // namespace snow_shot::storage

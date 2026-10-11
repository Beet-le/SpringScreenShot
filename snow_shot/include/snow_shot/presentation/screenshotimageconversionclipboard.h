#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONCLIPBOARD_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONCLIPBOARD_H

#include "snow_shot/presentation/screenshotimageconversion.h"

#include <QMimeData>
#include <QTextCursor>
#include <QTextDocumentFragment>

#include <memory>

namespace snow_shot::presentation {
[[nodiscard]] inline std::unique_ptr<QMimeData>
imageConversionClipboardMimeData(SnowShotImageConversionFormat format, const QString& source,
                                 const QTextCursor& cursor = {}) {
    if (source.isEmpty()) {
        return {};
    }
    auto mimeData = std::make_unique<QMimeData>();
    const bool selected = cursor.hasSelection();
    const QTextDocumentFragment fragment(cursor);
    // Source copies remain verbatim; preview selections export their rendered content.
    mimeData->setText(selected ? fragment.toPlainText() : source);
    if (format == SnowShotImageConversionFormat::Html) {
        // Qt maps text/html to the platform's dedicated native HTML clipboard format.
        mimeData->setHtml(selected ? fragment.toHtml() : source);
    }
    return mimeData;
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONCLIPBOARD_H

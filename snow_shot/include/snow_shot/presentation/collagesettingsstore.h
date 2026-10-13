// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H
#define SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H

#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

namespace snow_shot::presentation {

// Read once when a window opens. Persist only the edited field so an older window
// cannot overwrite more recent choices made in another independent window.
class CollageSettingsStore final {
  public:
    explicit CollageSettingsStore(storage::ConfigurationStore& configuration)
        : m_configuration(configuration) {}

    [[nodiscard]] CollageSettings read() const {
        CollageSettings result;
        result.direction = m_configuration.value(QStringLiteral("collage/direction")).toString() ==
                                   QStringLiteral("horizontal")
                               ? CollageDirection::Horizontal
                               : CollageDirection::Vertical;
        result.sizing = m_configuration.value(QStringLiteral("collage/sizing_mode")).toString() ==
                                QStringLiteral("original")
                            ? CollageSizing::Original
                            : CollageSizing::Uniform;
        result.border = m_configuration.value(QStringLiteral("collage/border_px")).toInt();
        result.spacing = m_configuration.value(QStringLiteral("collage/spacing_px")).toInt();
        result.background = storage::colorFromRgbaString(
            m_configuration.value(QStringLiteral("collage/background_color")).toString());
        if (!result.background.isValid())
            result.background = Qt::white;
        return result;
    }

    bool write(const QString& field, const CollageSettings& settings) const {
        QJsonValue value;
        if (field == QStringLiteral("direction"))
            value = settings.direction == CollageDirection::Vertical ? QStringLiteral("vertical")
                                                                     : QStringLiteral("horizontal");
        else if (field == QStringLiteral("sizing_mode"))
            value = settings.sizing == CollageSizing::Uniform ? QStringLiteral("uniform")
                                                              : QStringLiteral("original");
        else if (field == QStringLiteral("border_px"))
            value = settings.border;
        else if (field == QStringLiteral("spacing_px"))
            value = settings.spacing;
        else if (field == QStringLiteral("background_color"))
            value = storage::colorToRgbaString(settings.background);
        else
            return false;
        return m_configuration.setValue(QStringLiteral("collage/") + field, value);
    }

  private:
    storage::ConfigurationStore& m_configuration;
};

} // namespace snow_shot::presentation
#endif

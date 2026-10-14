// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H
#define SNOW_SHOT_PRESENTATION_COLLAGESETTINGSSTORE_H

#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QStringList>

namespace snow_shot::presentation {

// Read once when a window opens. Persist only the edited field so an older window
// cannot overwrite more recent choices made in another independent window.
class CollageSettingsStore final {
  public:
    explicit CollageSettingsStore(storage::ConfigurationStore& configuration)
        : m_configuration(configuration) {}

    [[nodiscard]] CollageSettings read() const {
        CollageSettings result;
        const QString direction =
            m_configuration.value(QStringLiteral("collage/direction")).toString();
        if (direction == QStringLiteral("custom"))
            result.direction = CollageDirection::Custom;
        else if (direction == QStringLiteral("horizontal"))
            result.direction = CollageDirection::Horizontal;
        result.sizing = m_configuration.value(QStringLiteral("collage/sizing_mode")).toString() ==
                                QStringLiteral("original")
                            ? CollageSizing::Original
                            : CollageSizing::Uniform;
        result.border = m_configuration.value(QStringLiteral("collage/border_px")).toInt();
        result.spacing = m_configuration.value(QStringLiteral("collage/spacing_px")).toInt();
        result.rows = m_configuration.value(QStringLiteral("collage/rows")).toInt();
        result.columns = m_configuration.value(QStringLiteral("collage/columns")).toInt();
        result.customGridInitialized =
            m_configuration.value(QStringLiteral("collage/custom_grid_initialized")).toBool();
        result.background = storage::colorFromRgbaString(
            m_configuration.value(QStringLiteral("collage/background_color")).toString());
        if (!result.background.isValid())
            result.background = Qt::white;
        return result;
    }

    bool write(const QString& field, const CollageSettings& settings) const {
        return write(QStringList{field}, settings);
    }

    bool write(const QStringList& fields, const CollageSettings& settings) const {
        QMap<QString, QJsonValue> values;
        for (const QString& field : fields) {
            QJsonValue value;
            if (field == QStringLiteral("direction")) {
                switch (settings.direction) {
                case CollageDirection::Vertical:
                    value = QStringLiteral("vertical");
                    break;
                case CollageDirection::Horizontal:
                    value = QStringLiteral("horizontal");
                    break;
                case CollageDirection::Custom:
                    value = QStringLiteral("custom");
                    break;
                }
            } else if (field == QStringLiteral("sizing_mode"))
                value = settings.sizing == CollageSizing::Uniform ? QStringLiteral("uniform")
                                                                  : QStringLiteral("original");
            else if (field == QStringLiteral("border_px"))
                value = settings.border;
            else if (field == QStringLiteral("spacing_px"))
                value = settings.spacing;
            else if (field == QStringLiteral("rows"))
                value = settings.rows;
            else if (field == QStringLiteral("columns"))
                value = settings.columns;
            else if (field == QStringLiteral("custom_grid_initialized"))
                value = settings.customGridInitialized;
            else if (field == QStringLiteral("background_color"))
                value = storage::colorToRgbaString(settings.background);
            else
                return false;
            values.insert(QStringLiteral("collage/") + field, value);
        }
        return m_configuration.setValues(values);
    }

  private:
    storage::ConfigurationStore& m_configuration;
};

} // namespace snow_shot::presentation
#endif

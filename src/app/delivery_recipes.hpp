// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

class QSettings;

namespace video_editor::app {

struct DeliveryRecipeEntry final {
  QString id;
  QString name;
  QString preset_id;
  QString primary_destination;
  QStringList extra_destinations;
  int resolution_index{0};
  int frame_rate_index{0};
  int caption_mode_index{0};
  bool use_export_range{false};
  bool prefer_hardware{false};
};

[[nodiscard]] std::vector<DeliveryRecipeEntry> loadDeliveryRecipes(const QSettings& settings);
void saveDeliveryRecipes(QSettings& settings, const std::vector<DeliveryRecipeEntry>& recipes);
void appendDeliveryRecipe(QSettings& settings, const DeliveryRecipeEntry& recipe);
[[nodiscard]] std::optional<DeliveryRecipeEntry> findDeliveryRecipe(const QSettings& settings,
                                                                    const QString& id);

} // namespace video_editor::app

// SPDX-License-Identifier: MPL-2.0

#include "delivery_recipes.hpp"

#include <QJsonArray>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>

namespace video_editor::app {

namespace {

constexpr auto kRecipesKey = "delivery/recipes";

std::vector<DeliveryRecipeEntry> parseRecipes(const QJsonArray& array) {
  std::vector<DeliveryRecipeEntry> recipes;
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    DeliveryRecipeEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.name = object.value(QStringLiteral("name")).toString();
    entry.preset_id = object.value(QStringLiteral("presetId")).toString();
    entry.primary_destination = object.value(QStringLiteral("primaryDestination")).toString();
    for (const QJsonValue& dest : object.value(QStringLiteral("extraDestinations")).toArray()) {
      entry.extra_destinations.push_back(dest.toString());
    }
    entry.resolution_index = object.value(QStringLiteral("resolutionIndex")).toInt(0);
    entry.frame_rate_index = object.value(QStringLiteral("frameRateIndex")).toInt(0);
    entry.caption_mode_index = object.value(QStringLiteral("captionModeIndex")).toInt(0);
    entry.use_export_range = object.value(QStringLiteral("useExportRange")).toBool(false);
    entry.prefer_hardware = object.value(QStringLiteral("preferHardware")).toBool(false);
    if (!entry.id.isEmpty() && !entry.name.isEmpty()) {
      recipes.push_back(std::move(entry));
    }
  }
  return recipes;
}

QJsonArray serializeRecipes(const std::vector<DeliveryRecipeEntry>& recipes) {
  QJsonArray array;
  for (const DeliveryRecipeEntry& entry : recipes) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("name"), entry.name);
    object.insert(QStringLiteral("presetId"), entry.preset_id);
    object.insert(QStringLiteral("primaryDestination"), entry.primary_destination);
    QJsonArray extras;
    for (const QString& dest : entry.extra_destinations) {
      extras.push_back(dest);
    }
    object.insert(QStringLiteral("extraDestinations"), extras);
    object.insert(QStringLiteral("resolutionIndex"), entry.resolution_index);
    object.insert(QStringLiteral("frameRateIndex"), entry.frame_rate_index);
    object.insert(QStringLiteral("captionModeIndex"), entry.caption_mode_index);
    object.insert(QStringLiteral("useExportRange"), entry.use_export_range);
    object.insert(QStringLiteral("preferHardware"), entry.prefer_hardware);
    array.push_back(object);
  }
  return array;
}

} // namespace

std::vector<DeliveryRecipeEntry> loadDeliveryRecipes(const QSettings& settings) {
  return parseRecipes(QJsonDocument::fromJson(settings.value(kRecipesKey).toByteArray()).array());
}

void saveDeliveryRecipes(QSettings& settings, const std::vector<DeliveryRecipeEntry>& recipes) {
  settings.setValue(kRecipesKey, QJsonDocument(serializeRecipes(recipes)).toJson(QJsonDocument::Compact));
}

void appendDeliveryRecipe(QSettings& settings, const DeliveryRecipeEntry& recipe) {
  std::vector<DeliveryRecipeEntry> recipes = loadDeliveryRecipes(settings);
  recipes.push_back(recipe);
  saveDeliveryRecipes(settings, recipes);
}

std::optional<DeliveryRecipeEntry> findDeliveryRecipe(const QSettings& settings,
                                                      const QString& id) {
  for (const DeliveryRecipeEntry& recipe : loadDeliveryRecipes(settings)) {
    if (recipe.id == id) {
      return recipe;
    }
  }
  return std::nullopt;
}

} // namespace video_editor::app

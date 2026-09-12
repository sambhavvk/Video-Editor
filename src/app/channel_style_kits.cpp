// SPDX-License-Identifier: MPL-2.0
#include "channel_style_kits.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

namespace video_editor::app {
namespace {

QJsonObject serializeStyle(const desktop_ui::CaptionStyleView& style) {
  QJsonObject object;
  object.insert(QStringLiteral("fontFamily"), style.fontFamily);
  object.insert(QStringLiteral("fontSize"), style.fontSize);
  object.insert(QStringLiteral("textColor"), style.textColor.name(QColor::HexArgb));
  object.insert(QStringLiteral("backgroundColor"), style.backgroundColor.name(QColor::HexArgb));
  object.insert(QStringLiteral("bold"), style.bold);
  object.insert(QStringLiteral("italic"), style.italic);
  object.insert(QStringLiteral("alignment"), style.alignment);
  object.insert(QStringLiteral("verticalPosition"), style.verticalPosition);
  object.insert(QStringLiteral("safeMargin"), style.safeMargin);
  object.insert(QStringLiteral("outlineWidth"), style.outlineWidth);
  object.insert(QStringLiteral("outlineColor"), style.outlineColor.name(QColor::HexArgb));
  return object;
}

desktop_ui::CaptionStyleView deserializeStyle(const QJsonObject& object) {
  desktop_ui::CaptionStyleView style;
  style.fontFamily = object.value(QStringLiteral("fontFamily")).toString(style.fontFamily);
  style.fontSize = object.value(QStringLiteral("fontSize")).toDouble(style.fontSize);
  style.textColor = QColor(object.value(QStringLiteral("textColor")).toString(style.textColor.name()));
  style.backgroundColor =
      QColor(object.value(QStringLiteral("backgroundColor")).toString(style.backgroundColor.name()));
  style.bold = object.value(QStringLiteral("bold")).toBool(style.bold);
  style.italic = object.value(QStringLiteral("italic")).toBool(style.italic);
  style.alignment = object.value(QStringLiteral("alignment")).toString(style.alignment);
  style.verticalPosition = object.value(QStringLiteral("verticalPosition")).toDouble(style.verticalPosition);
  style.safeMargin = object.value(QStringLiteral("safeMargin")).toDouble(style.safeMargin);
  style.outlineWidth = object.value(QStringLiteral("outlineWidth")).toDouble(style.outlineWidth);
  style.outlineColor =
      QColor(object.value(QStringLiteral("outlineColor")).toString(style.outlineColor.name()));
  return style;
}

} // namespace

std::vector<ChannelStyleKitEntry> defaultStyleKits() {
  ChannelStyleKitEntry lowerThird;
  lowerThird.id = QStringLiteral("builtin-lower-third");
  lowerThird.name = QStringLiteral("Lower third");
  lowerThird.kind = QStringLiteral("lower_third");
  lowerThird.titleText = QStringLiteral("Speaker name");
  lowerThird.titleFontFamily = QStringLiteral("Noto Sans");
  lowerThird.titleFontSize = 54.0;
  lowerThird.titleBold = true;
  lowerThird.captionStyle.fontFamily = QStringLiteral("Noto Sans");
  lowerThird.captionStyle.fontSize = 42.0;
  lowerThird.captionStyle.verticalPosition = 0.86;
  lowerThird.captionStyle.backgroundColor = QColor(0, 0, 0, 180);
  lowerThird.fontAttribution = QStringLiteral("Noto Sans (SIL Open Font License 1.1)");

  ChannelStyleKitEntry chapterCard;
  chapterCard.id = QStringLiteral("builtin-chapter-card");
  chapterCard.name = QStringLiteral("Chapter card");
  chapterCard.kind = QStringLiteral("chapter_card");
  chapterCard.titleText = QStringLiteral("Chapter title");
  chapterCard.titleFontFamily = QStringLiteral("Noto Sans");
  chapterCard.titleFontSize = 96.0;
  chapterCard.titleBold = true;
  chapterCard.captionStyle.fontFamily = QStringLiteral("Noto Sans");
  chapterCard.captionStyle.fontSize = 48.0;
  chapterCard.captionStyle.verticalPosition = 0.5;
  chapterCard.captionStyle.backgroundColor = QColor(0, 0, 0, 210);
  chapterCard.fontAttribution = QStringLiteral("Noto Sans (SIL Open Font License 1.1)");

  ChannelStyleKitEntry captionStyle;
  captionStyle.id = QStringLiteral("builtin-caption-style");
  captionStyle.name = QStringLiteral("Caption style");
  captionStyle.kind = QStringLiteral("caption_style");
  captionStyle.captionStyle.fontFamily = QStringLiteral("Noto Sans");
  captionStyle.captionStyle.fontSize = 44.0;
  captionStyle.captionStyle.outlineWidth = 2.0;
  captionStyle.captionStyle.outlineColor = Qt::black;
  captionStyle.captionStyle.safeMargin = 0.08;
  captionStyle.fontAttribution = QStringLiteral("Noto Sans (SIL Open Font License 1.1)");

  return {lowerThird, chapterCard, captionStyle};
}

std::vector<ChannelStyleKitEntry> loadChannelStyleKits(const QSettings& settings) {
  std::vector<ChannelStyleKitEntry> kits = defaultStyleKits();
  const QJsonDocument document =
      QJsonDocument::fromJson(settings.value(QStringLiteral("creator/styleKits")).toByteArray());
  if (!document.isArray()) {
    return kits;
  }
  for (const QJsonValue& value : document.array()) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    ChannelStyleKitEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.name = object.value(QStringLiteral("name")).toString();
    entry.kind = object.value(QStringLiteral("kind")).toString();
    entry.titleText = object.value(QStringLiteral("titleText")).toString();
    entry.titleFontFamily = object.value(QStringLiteral("titleFontFamily")).toString();
    entry.titleFontSize = object.value(QStringLiteral("titleFontSize")).toDouble(96.0);
    entry.titleBold = object.value(QStringLiteral("titleBold")).toBool(false);
    entry.titleItalic = object.value(QStringLiteral("titleItalic")).toBool(false);
    entry.fontAttribution = object.value(QStringLiteral("fontAttribution")).toString();
    if (object.contains(QStringLiteral("captionStyle"))) {
      entry.captionStyle = deserializeStyle(object.value(QStringLiteral("captionStyle")).toObject());
    }
    if (entry.id.isEmpty() || entry.name.isEmpty()) {
      continue;
    }
    kits.push_back(std::move(entry));
  }
  return kits;
}

void saveChannelStyleKits(QSettings& settings, const std::vector<ChannelStyleKitEntry>& kits) {
  QJsonArray saved;
  for (const ChannelStyleKitEntry& kit : kits) {
    if (kit.id.startsWith(QStringLiteral("builtin-"))) {
      continue;
    }
    QJsonObject object;
    object.insert(QStringLiteral("id"), kit.id);
    object.insert(QStringLiteral("name"), kit.name);
    object.insert(QStringLiteral("kind"), kit.kind);
    object.insert(QStringLiteral("titleText"), kit.titleText);
    object.insert(QStringLiteral("titleFontFamily"), kit.titleFontFamily);
    object.insert(QStringLiteral("titleFontSize"), kit.titleFontSize);
    object.insert(QStringLiteral("titleBold"), kit.titleBold);
    object.insert(QStringLiteral("titleItalic"), kit.titleItalic);
    object.insert(QStringLiteral("fontAttribution"), kit.fontAttribution);
    object.insert(QStringLiteral("captionStyle"), serializeStyle(kit.captionStyle));
    saved.append(object);
  }
  settings.setValue(QStringLiteral("creator/styleKits"), QJsonDocument(saved).toJson());
}

void appendChannelStyleKit(QSettings& settings, const ChannelStyleKitEntry& kit) {
  std::vector<ChannelStyleKitEntry> kits = loadChannelStyleKits(settings);
  kits.erase(std::remove_if(kits.begin(), kits.end(),
                            [&](const ChannelStyleKitEntry& entry) { return entry.id == kit.id; }),
             kits.end());
  kits.push_back(kit);
  saveChannelStyleKits(settings, kits);
}

std::optional<ChannelStyleKitEntry> findChannelStyleKit(const QSettings& settings,
                                                        const QString& id) {
  for (const ChannelStyleKitEntry& kit : loadChannelStyleKits(settings)) {
    if (kit.id == id) {
      return kit;
    }
  }
  return std::nullopt;
}

} // namespace video_editor::app

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QString>
#include <optional>
#include <vector>

class QSettings;

namespace video_editor::app {

struct ChannelStyleKitEntry final {
  QString id;
  QString name;
  QString kind;
  desktop_ui::CaptionStyleView captionStyle{};
  QString titleText;
  QString titleFontFamily;
  double titleFontSize{96.0};
  bool titleBold{false};
  bool titleItalic{false};
  QString fontAttribution;
};

[[nodiscard]] std::vector<ChannelStyleKitEntry> defaultStyleKits();
[[nodiscard]] std::vector<ChannelStyleKitEntry> loadChannelStyleKits(const QSettings& settings);
void saveChannelStyleKits(QSettings& settings, const std::vector<ChannelStyleKitEntry>& kits);
void appendChannelStyleKit(QSettings& settings, const ChannelStyleKitEntry& kit);
[[nodiscard]] std::optional<ChannelStyleKitEntry> findChannelStyleKit(const QSettings& settings,
                                                                      const QString& id);

} // namespace video_editor::app

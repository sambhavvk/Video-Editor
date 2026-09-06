// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

namespace video_editor::app {

inline constexpr int kMaxRecentProjectPaths = 10;

[[nodiscard]] QStringList readRecentProjectPaths(const QSettings& settings);
void addRecentProjectPath(QSettings& settings, const QString& absolutePath);
void pruneMissingRecentProjectPaths(QSettings& settings);
[[nodiscard]] bool reopenLastOnStartup(const QSettings& settings);
void setReopenLastOnStartup(QSettings& settings, bool enabled);
[[nodiscard]] QString lastRecentProjectPath(const QSettings& settings);

} // namespace video_editor::app

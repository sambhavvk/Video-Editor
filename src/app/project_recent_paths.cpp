// SPDX-License-Identifier: MPL-2.0
#include "project_recent_paths.hpp"

#include <QFileInfo>

namespace video_editor::app {

namespace {

constexpr auto kRecentPathsKey = "project/recentPaths";
constexpr auto kReopenLastKey = "project/reopenLastOnStartup";

[[nodiscard]] QString normalizedAbsolutePath(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  return QFileInfo(path).absoluteFilePath();
}

} // namespace

QStringList readRecentProjectPaths(const QSettings& settings) {
  return settings.value(QString::fromLatin1(kRecentPathsKey)).toStringList();
}

void addRecentProjectPath(QSettings& settings, const QString& absolutePath) {
  const QString normalized = normalizedAbsolutePath(absolutePath);
  if (normalized.isEmpty() || !QFileInfo::exists(normalized)) {
    return;
  }

  QStringList paths = readRecentProjectPaths(settings);
  paths.removeAll(normalized);
  paths.prepend(normalized);
  while (paths.size() > kMaxRecentProjectPaths) {
    paths.removeLast();
  }
  settings.setValue(QString::fromLatin1(kRecentPathsKey), paths);
  settings.sync();
}

void pruneMissingRecentProjectPaths(QSettings& settings) {
  QStringList paths = readRecentProjectPaths(settings);
  QStringList existing;
  existing.reserve(paths.size());
  for (const QString& path : paths) {
    if (QFileInfo::exists(path)) {
      existing.push_back(path);
    }
  }
  if (existing != paths) {
    settings.setValue(QString::fromLatin1(kRecentPathsKey), existing);
    settings.sync();
  }
}

bool reopenLastOnStartup(const QSettings& settings) {
  return settings.value(QString::fromLatin1(kReopenLastKey), true).toBool();
}

void setReopenLastOnStartup(QSettings& settings, const bool enabled) {
  settings.setValue(QString::fromLatin1(kReopenLastKey), enabled);
  settings.sync();
}

QString lastRecentProjectPath(const QSettings& settings) {
  const QStringList paths = readRecentProjectPaths(settings);
  return paths.isEmpty() ? QString{} : paths.front();
}

} // namespace video_editor::app

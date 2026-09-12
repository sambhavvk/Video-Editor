// SPDX-License-Identifier: MPL-2.0

#include "restore_points.hpp"

#include "path_utils.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <fstream>
#include <stdexcept>

namespace video_editor::app {

namespace {

std::filesystem::path manifestPath(const std::filesystem::path& recovery_directory) {
  return restorePointsDirectory(recovery_directory) / "manifest.json";
}

QString slugifyRestorePointName(const QString& name) {
  QString slug = name.trimmed().toLower();
  slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
  slug.replace(QRegularExpression(QStringLiteral("-+")), QStringLiteral("-"));
  if (slug.isEmpty()) {
    slug = QStringLiteral("restore-point");
  }
  return slug.left(48);
}

} // namespace

std::filesystem::path restorePointsDirectory(const std::filesystem::path& recovery_directory) {
  return recovery_directory / "restore-points";
}

std::vector<RestorePointEntry> loadNamedRestorePoints(
    const std::filesystem::path& recovery_directory) {
  std::vector<RestorePointEntry> entries;
  const std::filesystem::path manifest = manifestPath(recovery_directory);
  std::error_code exists_error;
  if (!std::filesystem::is_regular_file(manifest, exists_error) || exists_error) {
    return entries;
  }
  std::ifstream input(manifest, std::ios::binary);
  if (!input) {
    return entries;
  }
  const QJsonDocument document =
      QJsonDocument::fromJson(QByteArray::fromStdString(
          std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())));
  if (!document.isArray()) {
    return entries;
  }
  for (const QJsonValue& value : document.array()) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    RestorePointEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString().toStdString();
    entry.name = object.value(QStringLiteral("name")).toString().toStdString();
    entry.checkpoint = pathFromQString(object.value(QStringLiteral("checkpoint")).toString());
    entry.revision = static_cast<std::uint64_t>(
        object.value(QStringLiteral("revision")).toInteger(0));
    entry.sequence_name = object.value(QStringLiteral("sequenceName")).toString().toStdString();
    entry.created_utc_ms = object.value(QStringLiteral("createdUtcMs")).toInteger(0);
    entry.recovery_working_database = false;
    if (!entry.id.empty() && !entry.checkpoint.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

std::optional<RestorePointEntry> findRestorePoint(const std::filesystem::path& recovery_directory,
                                                  const std::string& id) {
  for (const RestorePointEntry& entry : loadNamedRestorePoints(recovery_directory)) {
    if (entry.id == id) {
      return entry;
    }
  }
  return std::nullopt;
}

void appendNamedRestorePoint(const std::filesystem::path& recovery_directory,
                             const RestorePointEntry& entry) {
  std::vector<RestorePointEntry> entries = loadNamedRestorePoints(recovery_directory);
  entries.push_back(entry);
  QJsonArray array;
  for (const RestorePointEntry& item : entries) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::fromStdString(item.id));
    object.insert(QStringLiteral("name"), QString::fromStdString(item.name));
    object.insert(QStringLiteral("checkpoint"), qStringFromPath(item.checkpoint));
    object.insert(QStringLiteral("revision"),
                  static_cast<qint64>(item.revision));
    object.insert(QStringLiteral("sequenceName"), QString::fromStdString(item.sequence_name));
    object.insert(QStringLiteral("createdUtcMs"), static_cast<qint64>(item.created_utc_ms));
    array.push_back(object);
  }
  const std::filesystem::path directory = restorePointsDirectory(recovery_directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path manifest = manifestPath(recovery_directory);
  const QByteArray payload = QJsonDocument(array).toJson(QJsonDocument::Compact);
  std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Could not open the restore-point manifest for writing");
  }
  output.write(payload.constData(), static_cast<std::streamsize>(payload.size()));
  output.flush();
  if (!output) {
    throw std::runtime_error("Could not write the restore-point manifest");
  }
}

std::filesystem::path makeRestorePointCheckpointPath(
    const std::filesystem::path& restore_points_directory, const QString& name,
    const std::uint64_t revision, const QString& unique_id) {
  QString token = unique_id;
  token.remove(QLatin1Char('-'));
  token.remove(QLatin1Char('/'));
  if (token.isEmpty()) {
    token = QStringLiteral("id");
  }
  const QString file_name = slugifyRestorePointName(name) + QStringLiteral("-r") +
                            QString::number(revision) + QStringLiteral("-") + token.left(16) +
                            QStringLiteral(".veproj");
  return restore_points_directory / pathFromQString(file_name);
}

} // namespace video_editor::app

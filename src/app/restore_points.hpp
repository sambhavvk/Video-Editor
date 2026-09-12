// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class QString;

namespace video_editor::app {

struct RestorePointEntry final {
  std::string id;
  std::string name;
  std::filesystem::path checkpoint;
  std::uint64_t revision{0};
  std::string sequence_name;
  std::int64_t created_utc_ms{0};
  bool recovery_working_database{false};
};

[[nodiscard]] std::filesystem::path restorePointsDirectory(
    const std::filesystem::path& recovery_directory);
[[nodiscard]] std::vector<RestorePointEntry> loadNamedRestorePoints(
    const std::filesystem::path& recovery_directory);
[[nodiscard]] std::optional<RestorePointEntry> findRestorePoint(
    const std::filesystem::path& recovery_directory, const std::string& id);
void appendNamedRestorePoint(const std::filesystem::path& recovery_directory,
                             const RestorePointEntry& entry);
[[nodiscard]] std::filesystem::path makeRestorePointCheckpointPath(
    const std::filesystem::path& restore_points_directory, const QString& name,
    std::uint64_t revision);

} // namespace video_editor::app

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/interchange/otio.h"

#include <filesystem>
#include <string>
#include <vector>

namespace video_editor::interchange {

struct OtioHandoffReport final {
  std::vector<std::string> supported;
  std::vector<std::string> baked;
  std::vector<std::string> flattened;
  std::vector<std::string> omitted;
  OtioReport otio;
};

struct OtioHandoffPackage final {
  std::filesystem::path package_root;
  std::filesystem::path timeline_path;
  std::filesystem::path report_path;
  std::filesystem::path manifest_path;
  OtioHandoffReport report;
};

// Build an OTIO handoff folder for downstream FOSS receivers (e.g. OpenTimelineIO tools).
[[nodiscard]] edit::Result<OtioHandoffPackage, std::string> build_otio_handoff_package(
    const edit::Project& project, edit::EntityId sequence_id,
    const std::filesystem::path& destination_directory, const std::string& receiver_label = {});

} // namespace video_editor::interchange

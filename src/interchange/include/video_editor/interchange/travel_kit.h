// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/project_codec/project_codec.h"

#include <filesystem>
#include <string>
#include <vector>

namespace video_editor::interchange {

struct TravelKitOptions final {
  bool include_proxies{false};
  bool include_fonts{false};
};

struct TravelKitManifest final {
  std::filesystem::path kit_root;
  std::filesystem::path project_path;
  std::filesystem::path manifest_path;
  std::vector<std::string> bundled_assets;
  std::vector<std::string> omitted_dependencies;
};

[[nodiscard]] edit::Result<TravelKitManifest, std::string>
build_travel_kit(const edit::Project& project, const project_codec::ProjectBytes& project_bytes,
                 const std::filesystem::path& destination_directory, TravelKitOptions options = {});

} // namespace video_editor::interchange

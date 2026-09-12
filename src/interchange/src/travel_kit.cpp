// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/travel_kit.h"

#include <fstream>

namespace video_editor::interchange {
namespace {

[[nodiscard]] bool is_relative_uri(const std::string& uri) {
  return uri.find("://") == std::string::npos;
}

} // namespace

edit::Result<TravelKitManifest, std::string> build_travel_kit(
    const edit::Project& project, const project_codec::ProjectBytes& project_bytes,
    const std::filesystem::path& destination_directory, const TravelKitOptions options) {
  if (destination_directory.empty()) {
    return edit::Result<TravelKitManifest, std::string>::failure("travel kit destination is empty");
  }
  std::error_code ec;
  std::filesystem::create_directories(destination_directory, ec);
  if (ec) {
    return edit::Result<TravelKitManifest, std::string>::failure("could not create travel kit folder");
  }
  const auto media_dir = destination_directory / "media";
  std::filesystem::create_directories(media_dir, ec);

  TravelKitManifest manifest;
  manifest.kit_root = destination_directory;
  manifest.project_path = destination_directory / "project.veproj";
  manifest.manifest_path = destination_directory / "asset_manifest.txt";

  {
    std::ofstream project_out(manifest.project_path, std::ios::binary);
    if (!project_out) {
      return edit::Result<TravelKitManifest, std::string>::failure("could not write project file");
    }
    project_out.write(reinterpret_cast<const char*>(project_bytes.data()),
                      static_cast<std::streamsize>(project_bytes.size()));
  }

  std::ofstream manifest_out(manifest.manifest_path);
  if (!manifest_out) {
    return edit::Result<TravelKitManifest, std::string>::failure("could not write asset manifest");
  }
  for (const edit::Asset& asset : project.assets) {
    manifest_out << asset.source_uri << '\n';
    if (asset.source_uri.empty()) {
      manifest.omitted_dependencies.push_back(asset.name + " (missing uri)");
      continue;
    }
    if (!is_relative_uri(asset.source_uri)) {
      manifest.bundled_assets.push_back(asset.source_uri);
      continue;
    }
    const std::filesystem::path source(asset.source_uri);
    if (!std::filesystem::exists(source)) {
      manifest.omitted_dependencies.push_back(asset.source_uri);
      continue;
    }
    const auto destination = media_dir / source.filename();
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::skip_existing, ec);
    if (ec) {
      manifest.omitted_dependencies.push_back(asset.source_uri);
    } else {
      manifest.bundled_assets.push_back(destination.generic_string());
    }
  }
  if (options.include_proxies) {
    manifest_out << "# proxies requested but not bundled in this kit\n";
  }
  if (options.include_fonts) {
    manifest_out << "# fonts: only redistributable assets should be added manually\n";
  }
  return edit::Result<TravelKitManifest, std::string>::success(std::move(manifest));
}

} // namespace video_editor::interchange

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/travel_kit.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace video_editor::interchange {
namespace {

[[nodiscard]] bool has_uri_scheme(const std::string& uri) {
  const auto separator = uri.find("://");
  return separator != std::string::npos && separator > 0;
}

[[nodiscard]] bool is_file_uri(const std::string& uri) {
  return uri.rfind("file://", 0) == 0;
}

[[nodiscard]] bool is_remote_uri(const std::string& uri) {
  return has_uri_scheme(uri) && !is_file_uri(uri);
}

[[nodiscard]] std::filesystem::path local_path_from_uri(const std::string& uri) {
  if (is_file_uri(uri)) {
    return std::filesystem::path(uri.substr(7));
  }
  return std::filesystem::path(uri);
}

[[nodiscard]] std::string kit_relative_media_uri(const std::filesystem::path& filename) {
  return (std::filesystem::path("media") / filename).generic_string();
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
  ec.clear();
  std::filesystem::create_directories(media_dir, ec);
  if (ec) {
    return edit::Result<TravelKitManifest, std::string>::failure("could not create travel kit media folder");
  }

  TravelKitManifest manifest;
  manifest.kit_root = destination_directory;
  manifest.project_path = destination_directory / "project.veproj";
  manifest.manifest_path = destination_directory / "asset_manifest.txt";

  edit::Project packaged = project;
  for (edit::Asset& asset : packaged.assets) {
    if (asset.source_uri.empty()) {
      manifest.omitted_dependencies.push_back(asset.name + " (missing uri)");
      continue;
    }
    if (is_remote_uri(asset.source_uri)) {
      manifest.omitted_dependencies.push_back(asset.source_uri);
      continue;
    }
    const std::filesystem::path source = local_path_from_uri(asset.source_uri);
    ec.clear();
    if (!std::filesystem::exists(source, ec) || ec) {
      manifest.omitted_dependencies.push_back(asset.source_uri);
      continue;
    }
    auto destination = media_dir / source.filename();
    if (std::filesystem::exists(destination, ec) && !ec) {
      destination = media_dir / (source.stem().string() + "_" + asset.id.toString() +
                                 source.extension().string());
    }
    ec.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      manifest.omitted_dependencies.push_back(asset.source_uri);
      continue;
    }
    const auto relative = kit_relative_media_uri(destination.filename());
    manifest.bundled_assets.push_back(relative);
    asset.source_uri = relative;
  }

  project_codec::ProjectBytes written_bytes = project_bytes;
  try {
    written_bytes = project_codec::serialize_project(packaged);
  } catch (const std::exception& exception) {
    return edit::Result<TravelKitManifest, std::string>::failure(
        std::string("could not serialize travel kit project: ") + exception.what());
  }

  {
    std::ofstream project_out(manifest.project_path, std::ios::binary);
    if (!project_out) {
      return edit::Result<TravelKitManifest, std::string>::failure("could not write project file");
    }
    project_out.write(reinterpret_cast<const char*>(written_bytes.data()),
                      static_cast<std::streamsize>(written_bytes.size()));
  }

  std::ofstream manifest_out(manifest.manifest_path);
  if (!manifest_out) {
    return edit::Result<TravelKitManifest, std::string>::failure("could not write asset manifest");
  }
  manifest_out << "Bundled:\n";
  for (const auto& entry : manifest.bundled_assets) {
    manifest_out << entry << '\n';
  }
  manifest_out << "Omitted:\n";
  for (const auto& entry : manifest.omitted_dependencies) {
    manifest_out << entry << '\n';
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

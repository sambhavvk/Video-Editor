// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/asset_service.h"
#include "video_editor/edit_model/commands.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/media_cache/cache_store.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::app {

struct MediaReconstructionOptions {
  std::optional<std::filesystem::path> legacy_proxy_directory;
  std::vector<std::filesystem::path> search_directories;
};

// Rebuilds a session AssetRecord from a persisted edit-model asset. Original
// media is probed when the path exists. Missing files may be recovered from
// search_directories when the filename and quick fingerprint match. Proxies
// are rediscovered from CacheStore / the optional legacy directory.
[[nodiscard]] assets::AssetRecord
reconstruct_asset_record(const edit::Asset& asset, media_cache::CacheStore* cache,
                         const MediaReconstructionOptions& options = {});

[[nodiscard]] std::vector<assets::AssetRecord>
reconstruct_media_records(const std::vector<edit::Asset>& assets, media_cache::CacheStore* cache,
                          const MediaReconstructionOptions& options = {});

[[nodiscard]] edit::RelinkAssetCommand relink_command_from_record(const assets::AssetRecord& record);

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& value);
[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& value);
[[nodiscard]] std::string filename_of_uri(const std::string& uri);

} // namespace video_editor::app

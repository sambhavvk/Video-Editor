// SPDX-License-Identifier: MPL-2.0
#include "media_reconstruction.hpp"

#include "video_editor/proxy_service/proxy_service.h"

#include <algorithm>
#include <system_error>

namespace video_editor::app {
namespace {

[[nodiscard]] assets::FileFingerprint fingerprint_from_asset(const edit::Asset& asset) {
  assets::FileFingerprint fingerprint;
  fingerprint.quick_sha256 = asset.fingerprint;
  return fingerprint;
}

[[nodiscard]] assets::AssetRecord stub_record(const edit::Asset& asset,
                                              assets::AssetAvailability availability) {
  assets::AssetRecord record;
  record.id = asset.id.toString();
  record.uri = path_from_utf8(asset.source_uri);
  record.fingerprint = fingerprint_from_asset(asset);
  record.availability = availability;
  return record;
}

void attach_proxy(assets::AssetRecord& record, media_cache::CacheStore* cache,
                  const MediaReconstructionOptions& options) {
  if (cache == nullptr || record.id.empty()) {
    return;
  }
  auto discovered = proxy::discover_proxy(record.id, record.fingerprint, *cache,
                                          options.legacy_proxy_directory);
  if (discovered.has_value() && discovered->manifest.complete) {
    record.proxy = std::move(discovered->manifest);
    record.proxy->pts_map_path = discovered->pts_map_path;
  }
}

[[nodiscard]] assets::AssetRecord adopt_import(const edit::Asset& asset,
                                               assets::AssetRecord imported) {
  imported.id = asset.id.toString();
  if (!asset.fingerprint.empty() &&
      imported.fingerprint.quick_sha256 != asset.fingerprint) {
    imported.availability = assets::AssetAvailability::Changed;
  } else {
    imported.availability = assets::AssetAvailability::Online;
  }
  imported.proxy.reset();
  return imported;
}

[[nodiscard]] std::optional<assets::AssetRecord>
try_import_path(const edit::Asset& asset, const std::filesystem::path& path) {
  std::error_code error;
  if (path.empty() || !std::filesystem::is_regular_file(path, error) || error) {
    return std::nullopt;
  }
  assets::AssetService service;
  auto imported = service.import(path);
  if (!imported) {
    return std::nullopt;
  }
  return adopt_import(asset, std::move(imported).value());
}

[[nodiscard]] std::optional<assets::AssetRecord>
try_recover(const edit::Asset& asset, const MediaReconstructionOptions& options) {
  const std::string filename = filename_of_uri(asset.source_uri);
  if (filename.empty()) {
    return std::nullopt;
  }
  for (const auto& directory : options.search_directories) {
    if (directory.empty()) {
      continue;
    }
    auto recovered = try_import_path(asset, directory / filename);
    if (!recovered.has_value()) {
      continue;
    }
    if (recovered->availability != assets::AssetAvailability::Online) {
      continue;
    }
    return recovered;
  }
  return std::nullopt;
}

} // namespace

std::filesystem::path path_from_utf8(const std::string& value) {
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
}

std::string utf8_from_path(const std::filesystem::path& value) {
  const auto utf8 = value.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string filename_of_uri(const std::string& uri) {
  const auto separator = uri.find_last_of("/\\");
  return separator == std::string::npos ? uri : uri.substr(separator + 1);
}

assets::AssetRecord reconstruct_asset_record(const edit::Asset& asset,
                                             media_cache::CacheStore* cache,
                                             const MediaReconstructionOptions& options) {
  const std::filesystem::path original = path_from_utf8(asset.source_uri);
  if (auto online = try_import_path(asset, original)) {
    attach_proxy(*online, cache, options);
    return std::move(*online);
  }

  if (auto recovered = try_recover(asset, options)) {
    attach_proxy(*recovered, cache, options);
    return std::move(*recovered);
  }

  assets::AssetRecord missing = stub_record(asset, assets::AssetAvailability::Missing);
  attach_proxy(missing, cache, options);
  return missing;
}

std::vector<assets::AssetRecord>
reconstruct_media_records(const std::vector<edit::Asset>& assets, media_cache::CacheStore* cache,
                          const MediaReconstructionOptions& options) {
  std::vector<assets::AssetRecord> records;
  records.reserve(assets.size());
  for (const edit::Asset& asset : assets) {
    records.push_back(reconstruct_asset_record(asset, cache, options));
  }
  return records;
}

edit::RelinkAssetCommand relink_command_from_record(const assets::AssetRecord& record) {
  edit::RelinkAssetCommand command;
  const auto parsed = edit::EntityId::parse(record.id);
  command.asset_id = parsed.value_or(edit::EntityId{});
  command.source_uri = utf8_from_path(record.uri);
  command.fingerprint = record.fingerprint.quick_sha256;
  if (record.descriptor.duration_microseconds.has_value() &&
      *record.descriptor.duration_microseconds > 0) {
    command.duration = edit::Time(*record.descriptor.duration_microseconds, 1'000'000);
  }
  for (const auto& stream : record.descriptor.streams) {
    if (stream.video.has_value() && !command.has_video) {
      command.has_video = true;
      command.width = static_cast<std::uint32_t>(std::max(stream.video->width, 0));
      command.height = static_cast<std::uint32_t>(std::max(stream.video->height, 0));
      command.metadata["video_codec"] = stream.codec_name;
      if (stream.video->average_frame_rate.numerator > 0 &&
          stream.video->average_frame_rate.denominator > 0) {
        command.nominal_frame_rate =
            edit::Rate(static_cast<std::uint32_t>(stream.video->average_frame_rate.numerator),
                       static_cast<std::uint32_t>(stream.video->average_frame_rate.denominator));
      }
    }
    if (stream.audio.has_value() && !command.has_audio) {
      command.has_audio = true;
      command.audio_sample_rate = static_cast<std::uint32_t>(stream.audio->sample_rate);
      command.audio_channels = static_cast<std::uint32_t>(stream.audio->channels);
      command.metadata["audio_codec"] = stream.codec_name;
    }
  }
  command.metadata["container"] = record.descriptor.format_name;
  return command;
}

} // namespace video_editor::app

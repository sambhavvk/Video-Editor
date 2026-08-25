// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"
#include "video_editor/proxy_service/proxy_service.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace video_editor::playback {

struct AssetStreamLocation final {
  std::filesystem::path path;
  // A negative index asks FFmpeg to select the best video stream.
  int video_stream_index{-1};

  friend bool operator==(const AssetStreamLocation&, const AssetStreamLocation&) = default;
};

struct AssetPlaybackSources final {
  AssetStreamLocation original;
  std::optional<AssetStreamLocation> proxy;
  std::optional<std::filesystem::path> pts_map_path;
};

struct ResolvedAssetStream final {
  AssetStreamLocation location;
  bool is_proxy{false};
  std::uint64_t registry_generation{0};
  std::shared_ptr<const proxy::PtsMap> pts_map;
};

// Thread-safe mapping from edit-model asset IDs to authoritative originals and
// optional time-aligned proxies. Registration does not open or probe media.
class AssetRegistry final {
public:
  AssetRegistry();
  ~AssetRegistry();

  AssetRegistry(const AssetRegistry&) = delete;
  AssetRegistry& operator=(const AssetRegistry&) = delete;
  AssetRegistry(AssetRegistry&&) noexcept;
  AssetRegistry& operator=(AssetRegistry&&) noexcept;

  // Returns false for a nil ID, an empty original path, or stream indexes below
  // -1. Re-registering an ID atomically replaces its locations.
  [[nodiscard]] bool register_asset(edit::EntityId asset_id, AssetPlaybackSources sources);
  [[nodiscard]] bool unregister_asset(const edit::EntityId& asset_id);

  // A proxy is selected only when requested, the proxy file is present, and a
  // loadable `.vepts` map was validated at registration. Otherwise playback
  // transparently falls back to the original.
  [[nodiscard]] std::optional<ResolvedAssetStream> resolve(const edit::EntityId& asset_id,
                                                           bool permit_proxy) const;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::playback

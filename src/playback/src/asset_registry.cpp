// SPDX-License-Identifier: MPL-2.0
#include "video_editor/playback/asset_registry.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace video_editor::playback {
namespace {

[[nodiscard]] bool valid_location(const AssetStreamLocation& location) {
  return !location.path.empty() && location.video_stream_index >= -1;
}

[[nodiscard]] std::filesystem::path normalized_path(const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    return path.lexically_normal();
  }
  return absolute.lexically_normal();
}

[[nodiscard]] std::shared_ptr<const proxy::PtsMap>
load_validated_pts_map(const std::optional<std::filesystem::path>& path) {
  if (!path.has_value() || path->empty()) {
    return nullptr;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(*path, error) || error) {
    return nullptr;
  }
  const auto loaded = proxy::load_pts_map(*path);
  if (!loaded) {
    return nullptr;
  }
  return std::make_shared<const proxy::PtsMap>(std::move(loaded).value());
}

[[nodiscard]] bool proxy_is_playable(const AssetPlaybackSources& sources,
                                     const std::shared_ptr<const proxy::PtsMap>& pts_map) {
  if (!sources.proxy.has_value() || pts_map == nullptr) {
    return false;
  }
  std::error_code error;
  return std::filesystem::is_regular_file(sources.proxy->path, error) && !error;
}

} // namespace

class AssetRegistry::Impl final {
public:
  struct Entry final {
    AssetPlaybackSources sources;
    std::shared_ptr<const proxy::PtsMap> pts_map;
    std::uint64_t generation{0};
  };

  mutable std::shared_mutex mutex;
  std::unordered_map<edit::EntityId, Entry> entries;
  std::uint64_t next_generation{1};
};

AssetRegistry::AssetRegistry() : impl_(std::make_unique<Impl>()) {}

AssetRegistry::~AssetRegistry() = default;

AssetRegistry::AssetRegistry(AssetRegistry&&) noexcept = default;

AssetRegistry& AssetRegistry::operator=(AssetRegistry&&) noexcept = default;

bool AssetRegistry::register_asset(const edit::EntityId asset_id, AssetPlaybackSources sources) {
  if (asset_id.isNil() || !valid_location(sources.original) ||
      (sources.proxy.has_value() && !valid_location(*sources.proxy))) {
    return false;
  }

  sources.original.path = normalized_path(sources.original.path);
  if (sources.proxy.has_value()) {
    sources.proxy->path = normalized_path(sources.proxy->path);
  }
  if (sources.pts_map_path.has_value() && !sources.pts_map_path->empty()) {
    sources.pts_map_path = normalized_path(*sources.pts_map_path);
  } else {
    sources.pts_map_path.reset();
  }

  std::shared_ptr<const proxy::PtsMap> pts_map;
  if (sources.proxy.has_value()) {
    pts_map = load_validated_pts_map(sources.pts_map_path);
    if (pts_map == nullptr) {
      sources.proxy.reset();
      sources.pts_map_path.reset();
    }
  }

  std::unique_lock lock(impl_->mutex);
  const std::uint64_t generation = impl_->next_generation++;
  impl_->entries.insert_or_assign(
      asset_id,
      Impl::Entry{.sources = std::move(sources), .pts_map = std::move(pts_map), .generation = generation});
  return true;
}

bool AssetRegistry::unregister_asset(const edit::EntityId& asset_id) {
  std::unique_lock lock(impl_->mutex);
  return impl_->entries.erase(asset_id) != 0U;
}

std::optional<ResolvedAssetStream> AssetRegistry::resolve(const edit::EntityId& asset_id,
                                                          const bool permit_proxy) const {
  std::shared_lock lock(impl_->mutex);
  const auto iterator = impl_->entries.find(asset_id);
  if (iterator == impl_->entries.end()) {
    return std::nullopt;
  }

  const Impl::Entry& entry = iterator->second;
  if (permit_proxy && proxy_is_playable(entry.sources, entry.pts_map)) {
    return ResolvedAssetStream{.location = *entry.sources.proxy,
                               .is_proxy = true,
                               .registry_generation = entry.generation,
                               .pts_map = entry.pts_map};
  }
  return ResolvedAssetStream{.location = entry.sources.original,
                             .is_proxy = false,
                             .registry_generation = entry.generation,
                             .pts_map = nullptr};
}

std::size_t AssetRegistry::size() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->entries.size();
}

std::uint64_t AssetRegistry::generation() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->next_generation;
}

} // namespace video_editor::playback

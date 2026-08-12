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

} // namespace

class AssetRegistry::Impl final {
public:
  struct Entry final {
    AssetPlaybackSources sources;
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

  std::unique_lock lock(impl_->mutex);
  const std::uint64_t generation = impl_->next_generation++;
  impl_->entries.insert_or_assign(
      asset_id, Impl::Entry{.sources = std::move(sources), .generation = generation});
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
  if (permit_proxy && entry.sources.proxy.has_value()) {
    std::error_code error;
    if (std::filesystem::is_regular_file(entry.sources.proxy->path, error) && !error) {
      return ResolvedAssetStream{.location = *entry.sources.proxy,
                                 .is_proxy = true,
                                 .registry_generation = entry.generation};
    }
  }
  return ResolvedAssetStream{.location = entry.sources.original,
                             .is_proxy = false,
                             .registry_generation = entry.generation};
}

std::size_t AssetRegistry::size() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->entries.size();
}

} // namespace video_editor::playback

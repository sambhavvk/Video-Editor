// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/original_audio_registry.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace video_editor::audio_render {
namespace {

[[nodiscard]] std::filesystem::path normalized_path(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  return error ? path.lexically_normal() : absolute.lexically_normal();
}

} // namespace

class OriginalAudioRegistry::Impl final {
public:
  mutable std::shared_mutex mutex;
  std::unordered_map<edit::EntityId, OriginalAudioMedia> entries;
};

OriginalAudioRegistry::OriginalAudioRegistry() : impl_(std::make_unique<Impl>()) {}

OriginalAudioRegistry::~OriginalAudioRegistry() = default;

OriginalAudioRegistry::OriginalAudioRegistry(OriginalAudioRegistry&&) noexcept = default;

OriginalAudioRegistry& OriginalAudioRegistry::operator=(OriginalAudioRegistry&&) noexcept = default;

bool OriginalAudioRegistry::register_original(const edit::EntityId asset_id,
                                              OriginalAudioMedia media) {
  if (asset_id.isNil() || media.path.empty() || media.audio_stream_index < -1) {
    return false;
  }
  media.path = normalized_path(media.path);
  std::unique_lock lock(impl_->mutex);
  impl_->entries.insert_or_assign(asset_id, std::move(media));
  return true;
}

bool OriginalAudioRegistry::unregister_asset(const edit::EntityId& asset_id) {
  std::unique_lock lock(impl_->mutex);
  return impl_->entries.erase(asset_id) != 0U;
}

std::optional<OriginalAudioMedia>
OriginalAudioRegistry::resolve_original(const edit::EntityId& asset_id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->entries.find(asset_id);
  return found == impl_->entries.end() ? std::nullopt
                                       : std::optional<OriginalAudioMedia>{found->second};
}

std::size_t OriginalAudioRegistry::size() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->entries.size();
}

} // namespace video_editor::audio_render

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace video_editor::audio_render {

// An authoritative media location. This type deliberately has no proxy field:
// timeline audio export must always resolve an original.
struct OriginalAudioMedia final {
  std::filesystem::path path;
  // A negative index asks FFmpeg to select the best audio stream.
  int audio_stream_index{-1};

  friend bool operator==(const OriginalAudioMedia&, const OriginalAudioMedia&) = default;
};

class OriginalAudioProvider {
public:
  virtual ~OriginalAudioProvider() = default;

  [[nodiscard]] virtual std::optional<OriginalAudioMedia>
  resolve_original(const edit::EntityId& asset_id) const = 0;
};

// Thread-safe provider for desktop/export integration. Registration performs
// no media I/O, so callers may build a registry on the UI thread.
class OriginalAudioRegistry final : public OriginalAudioProvider {
public:
  OriginalAudioRegistry();
  ~OriginalAudioRegistry() override;

  OriginalAudioRegistry(const OriginalAudioRegistry&) = delete;
  OriginalAudioRegistry& operator=(const OriginalAudioRegistry&) = delete;
  OriginalAudioRegistry(OriginalAudioRegistry&&) noexcept;
  OriginalAudioRegistry& operator=(OriginalAudioRegistry&&) noexcept;

  [[nodiscard]] bool register_original(edit::EntityId asset_id, OriginalAudioMedia media);
  [[nodiscard]] bool unregister_asset(const edit::EntityId& asset_id);
  [[nodiscard]] std::optional<OriginalAudioMedia>
  resolve_original(const edit::EntityId& asset_id) const override;
  [[nodiscard]] std::size_t size() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio_render

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace video_editor::audio_render {

inline constexpr std::uint32_t kTimelineAudioSampleRate = 48'000;
inline constexpr std::uint32_t kTimelineAudioChannels = 2;

enum class AudioRenderErrorCode {
  InvalidRequest,
  InvalidTimeline,
  Cancelled,
  MissingAsset,
  MissingMedia,
  CannotOpenMedia,
  AudioStreamNotFound,
  DecoderUnavailable,
  DecodeFailed,
  ResampleFailed,
};

struct AudioRenderError final {
  AudioRenderErrorCode code{AudioRenderErrorCode::InvalidRequest};
  std::string message;
  std::optional<edit::EntityId> asset_id;
  std::optional<edit::EntityId> clip_id;
};

struct AudioRenderRequest final {
  // Absolute sample position on the sequence's 48 kHz master timeline.
  std::int64_t start_sample{0};
  std::size_t sample_count{0};
  std::stop_token cancellation{};
};

using AudioRenderResult = edit::Result<audio::AudioBlock, AudioRenderError>;

// Pull renderer intended for decode/render threads and offline export. render()
// performs file I/O, decoding, resampling, and allocation; it must never be
// called from an audio device callback. Each request is self-contained, which
// makes repeated export requests deterministic and safe across revisions.
class TimelineAudioRenderer final {
public:
  explicit TimelineAudioRenderer(std::shared_ptr<const OriginalAudioProvider> originals);
  ~TimelineAudioRenderer();

  TimelineAudioRenderer(const TimelineAudioRenderer&) = delete;
  TimelineAudioRenderer& operator=(const TimelineAudioRenderer&) = delete;
  TimelineAudioRenderer(TimelineAudioRenderer&&) noexcept;
  TimelineAudioRenderer& operator=(TimelineAudioRenderer&&) noexcept;

  [[nodiscard]] AudioRenderResult render(const edit::TimelineSnapshot& snapshot,
                                         const AudioRenderRequest& request) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio_render

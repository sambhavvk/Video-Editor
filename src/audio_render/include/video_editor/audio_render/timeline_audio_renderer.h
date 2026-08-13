// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

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

struct TrackMeterReading final {
  edit::EntityId track_id{};
  std::array<float, 2> peak{0.0F, 0.0F};
  std::array<float, 2> rms{0.0F, 0.0F};
  bool active{false};
};

struct TrackMeterSnapshot final {
  std::uint64_t version{0};
  edit::Revision revision{};
  // Half-open source-timeline range represented by this render-stage tap.
  std::int64_t start_sample{0};
  std::int64_t end_sample{0};
  bool stale{true};
  std::vector<TrackMeterReading> tracks;
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

  // Returns the latest bounded per-track post-DSP meter snapshot. Track IDs
  // are authoritative; callers must not map these readings by track index.
  // The snapshot is produced on the render worker and read from the UI thread.
  [[nodiscard]] TrackMeterSnapshot trackMeters() const;

  // Selects the bounded rendered block containing the audio-master position.
  // A position outside retained ranges returns the latest track identity set
  // marked stale, never a future pre-rendered block.
  [[nodiscard]] TrackMeterSnapshot trackMetersAt(std::int64_t sample_counter) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio_render

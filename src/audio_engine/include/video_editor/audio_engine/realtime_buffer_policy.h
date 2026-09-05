// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/realtime_playback.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace video_editor::audio {

// User-facing decode-ahead profiles. Small trades latency for headroom; Large
// is the xrun-resistant ceiling used by adaptive growth after underruns.
enum class RealtimeBufferSize : std::uint8_t {
  Small = 0,
  Medium = 1,
  Large = 2,
};

inline constexpr std::uint8_t kRealtimeBufferSizeCount = 3;
inline constexpr std::uint32_t kPlaybackSampleRateHz = 48'000;

struct RealtimeBufferProfile final {
  std::size_t ring_capacity_frames{192'000};
  std::size_t render_block_frames{24'000};
  std::size_t prefill_frames{48'000};
};

[[nodiscard]] constexpr RealtimeBufferSize clamp_realtime_buffer_size(const int value) noexcept {
  if (value <= static_cast<int>(RealtimeBufferSize::Small)) {
    return RealtimeBufferSize::Small;
  }
  if (value >= static_cast<int>(RealtimeBufferSize::Large)) {
    return RealtimeBufferSize::Large;
  }
  return static_cast<RealtimeBufferSize>(value);
}

[[nodiscard]] constexpr RealtimeBufferProfile profile_for(const RealtimeBufferSize size) noexcept {
  switch (size) {
  case RealtimeBufferSize::Small:
    return {.ring_capacity_frames = 96'000,
            .render_block_frames = 12'000,
            .prefill_frames = 24'000};
  case RealtimeBufferSize::Large:
    return {.ring_capacity_frames = 384'000,
            .render_block_frames = 24'000,
            .prefill_frames = 96'000};
  case RealtimeBufferSize::Medium:
    break;
  }
  return {.ring_capacity_frames = 192'000,
          .render_block_frames = 24'000,
          .prefill_frames = 48'000};
}

[[nodiscard]] constexpr RealtimeBufferSize grow_realtime_buffer_size(
    const RealtimeBufferSize current) noexcept {
  if (current == RealtimeBufferSize::Small) {
    return RealtimeBufferSize::Medium;
  }
  return RealtimeBufferSize::Large;
}

[[nodiscard]] constexpr RealtimeBufferSize effective_realtime_buffer_size(
    const RealtimeBufferSize user_size, const std::uint8_t adaptive_boost) noexcept {
  RealtimeBufferSize size = user_size;
  for (std::uint8_t step = 0; step < adaptive_boost && size != RealtimeBufferSize::Large; ++step) {
    size = grow_realtime_buffer_size(size);
  }
  return size;
}

[[nodiscard]] constexpr double frames_to_milliseconds(
    const std::uint64_t frames, const std::uint32_t sample_rate = kPlaybackSampleRateHz) noexcept {
  if (sample_rate == 0U) {
    return 0.0;
  }
  return (static_cast<double>(frames) * 1000.0) / static_cast<double>(sample_rate);
}

// Absolute error between the audio-master sample counter and wall-clock time
// since the same origin. Used by the HUD and the physical A/V lab harness.
[[nodiscard]] inline double av_clock_error_milliseconds(
    const std::int64_t sample_counter, const double wall_seconds,
    const std::int64_t origin_sample = 0,
    const std::uint32_t sample_rate = kPlaybackSampleRateHz) noexcept {
  if (sample_rate == 0U || !std::isfinite(wall_seconds)) {
    return 0.0;
  }
  const double audio_seconds =
      static_cast<double>(sample_counter - origin_sample) / static_cast<double>(sample_rate);
  return std::abs(audio_seconds - wall_seconds) * 1000.0;
}

[[nodiscard]] inline RealtimePlaybackConfiguration configuration_for_buffer_size(
    const RealtimeBufferSize size, std::string device_id = {},
    const std::optional<std::uint64_t> calibrated_latency_frames = std::nullopt) {
  const RealtimeBufferProfile profile = profile_for(size);
  return RealtimePlaybackConfiguration{
      .ring_capacity_frames = profile.ring_capacity_frames,
      .render_block_frames = profile.render_block_frames,
      .prefill_frames = profile.prefill_frames,
      .prefill_timeout = std::chrono::milliseconds(2'000),
      .device_id = std::move(device_id),
      .calibrated_latency_frames = calibrated_latency_frames,
  };
}

} // namespace video_editor::audio

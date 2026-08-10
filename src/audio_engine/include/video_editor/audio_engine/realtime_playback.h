// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/audio_output_device.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

namespace video_editor::audio {

inline constexpr AudioFormat kPlaybackAudioFormat{.sample_rate = 48'000, .channels = 2};

struct PlaybackRenderRequest final {
  std::int64_t start_sample{0};
  std::size_t sample_count{0};
  std::uint64_t epoch{0};
  std::stop_token cancellation{};
};

enum class PlaybackRenderStatus : std::uint8_t {
  Ready,
  EndOfStream,
  Cancelled,
  Failed,
};

struct PlaybackRenderResult final {
  PlaybackRenderStatus status{PlaybackRenderStatus::Failed};
  std::optional<AudioBlock> block{};
  std::string message;

  [[nodiscard]] static PlaybackRenderResult ready(AudioBlock block_value) {
    return {.status = PlaybackRenderStatus::Ready, .block = std::move(block_value), .message = {}};
  }
  [[nodiscard]] static PlaybackRenderResult end_of_stream() {
    return {.status = PlaybackRenderStatus::EndOfStream, .block = std::nullopt, .message = {}};
  }
  [[nodiscard]] static PlaybackRenderResult cancelled(std::string message_value = {}) {
    return {.status = PlaybackRenderStatus::Cancelled,
            .block = std::nullopt,
            .message = std::move(message_value)};
  }
  [[nodiscard]] static PlaybackRenderResult failure(std::string message_value) {
    return {.status = PlaybackRenderStatus::Failed,
            .block = std::nullopt,
            .message = std::move(message_value)};
  }
};

// Adapter boundary for TimelineAudioRenderer or another immutable-revision
// source. render() runs only on the pre-render worker and may allocate, lock,
// decode, and perform I/O. A Ready result must cover the exact requested range;
// implementations must observe cancellation so pause, seek, and stop can join.
class PlaybackAudioProvider {
public:
  virtual ~PlaybackAudioProvider() = default;
  [[nodiscard]] virtual PlaybackRenderResult render(const PlaybackRenderRequest& request) = 0;
};

struct RealtimePlaybackConfiguration final {
  std::size_t ring_capacity_frames{96'000};
  std::size_t render_block_frames{960};
  std::size_t prefill_frames{4'800};
  std::chrono::milliseconds prefill_timeout{2'000};
};

enum class PlaybackState : std::uint8_t {
  Stopped,
  Starting,
  Playing,
  Paused,
  Failed,
};

enum class PlaybackControlErrorCode : std::uint8_t {
  InvalidConfiguration,
  InvalidState,
  InvalidSample,
  DeviceOpenFailed,
  DeviceStartFailed,
  ProviderFailed,
  PrefillTimeout,
  Cancelled,
};

struct PlaybackControlError final {
  PlaybackControlErrorCode code{PlaybackControlErrorCode::InvalidState};
  std::string message;
  std::optional<AudioDeviceError> device_error{};
};

struct PlaybackControlResult final {
  std::optional<PlaybackControlError> error{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !error.has_value();
  }
  [[nodiscard]] static PlaybackControlResult success() noexcept {
    return {};
  }
  [[nodiscard]] static PlaybackControlResult failure(PlaybackControlError error_value) {
    return {.error = std::move(error_value)};
  }
};

struct PlaybackDiagnostics final {
  PlaybackState state{PlaybackState::Stopped};
  // Conservative, latency-compensated master clock position. This is the
  // value returned by sample_counter().
  std::int64_t playback_position_sample{0};
  std::int64_t sample_counter{0};
  // End of the most recently submitted output buffer, not an audible clock.
  std::int64_t submitted_sample_counter{0};
  std::uint64_t estimated_output_latency_frames{0};
  std::uint64_t clock_uncertainty_frames{0};
  bool clock_is_estimated{true};
  std::uint64_t epoch{0};
  std::uint64_t callback_count{0};
  std::uint64_t pre_rendered_frames{0};
  std::uint64_t xrun_count{0};
  std::uint64_t underrun_frames{0};
  std::size_t queued_frames{0};
  bool provider_exhausted{false};
  bool device_present{false};
  bool device_open{false};
  bool device_running{false};
  std::string last_error;
};

class RealtimeAudioPlayback final {
public:
  explicit RealtimeAudioPlayback(std::shared_ptr<PlaybackAudioProvider> provider,
                                 RealtimePlaybackConfiguration configuration = {},
                                 std::unique_ptr<AudioOutputDevice> device = nullptr);
  ~RealtimeAudioPlayback();

  RealtimeAudioPlayback(const RealtimeAudioPlayback&) = delete;
  RealtimeAudioPlayback& operator=(const RealtimeAudioPlayback&) = delete;

  [[nodiscard]] PlaybackControlResult start(std::int64_t start_sample = 0);
  [[nodiscard]] PlaybackControlResult pause();
  [[nodiscard]] PlaybackControlResult resume();
  [[nodiscard]] PlaybackControlResult seek(std::int64_t sample);
  // Quiesce any current transport, establish an exact position, and leave the
  // controller resumable without starting the output device. This synchronous
  // primitive lets the async intent reconciler collapse Start/Seek followed by
  // Pause without briefly exposing stale audio.
  [[nodiscard]] PlaybackControlResult prepare_paused(std::int64_t sample);
  void stop() noexcept;

  // Manual callback entry for no-device hosts. It is noexcept,
  // allocation-free, lock-free, and never invokes the provider. Calls are
  // rejected while a physical device is present, and overlapping manual calls
  // return zeroes without becoming a second ring consumer. Seek and stop
  // synchronously quiesce an admitted manual callback before resetting the
  // ring. It returns frames obtained from the ring; the rest are deterministic
  // zeroes and count as one xrun event.
  [[nodiscard]] std::size_t render_callback(std::span<float> interleaved_output) noexcept;

  [[nodiscard]] PlaybackState state() const noexcept;
  // Conservative latency-compensated playback position. It never reports the
  // end of the buffer currently being submitted to a device.
  [[nodiscard]] std::int64_t sample_counter() const noexcept;
  [[nodiscard]] std::int64_t submitted_sample_counter() const noexcept;
  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept;
  // Diagnostic convenience only; sample_counter() is the canonical clock.
  [[nodiscard]] double clock_seconds() const noexcept;
  [[nodiscard]] std::uint64_t epoch() const noexcept;
  [[nodiscard]] bool device_present() const noexcept;
  [[nodiscard]] bool device_open() const noexcept;
  [[nodiscard]] PlaybackDiagnostics diagnostics() const;

  // Async control infrastructure can interrupt a blocking prefill/provider
  // pull without entering the control mutex. Direct synchronous users do not
  // normally need these methods.
  void request_control_cancellation() noexcept;
  void clear_control_cancellation() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio

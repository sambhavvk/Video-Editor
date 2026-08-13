// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/realtime_playback.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace video_editor::audio {

enum class PlaybackCommandKind : std::uint8_t { Start, Pause, Resume, Seek, Stop };

enum class PlaybackCommandStatus : std::uint8_t { Pending, Succeeded, Failed };

enum class PlaybackCommandEnqueueErrorCode : std::uint8_t {
  QueueFull,
  ShuttingDown,
  InvalidSample,
};

struct PlaybackCommandEnqueueError final {
  PlaybackCommandEnqueueErrorCode code{PlaybackCommandEnqueueErrorCode::QueueFull};
  std::string message;
};

struct PlaybackCommandReceipt final {
  std::uint64_t version{0};
  bool accepted{false};
  std::optional<PlaybackCommandEnqueueError> error{};
};

struct AsyncRealtimePlaybackConfiguration final {
  std::size_t maximum_pending_commands{64};
};

struct AsyncPlaybackDiagnostics final {
  PlaybackState requested_state{PlaybackState::Stopped};
  PlaybackState effective_state{PlaybackState::Stopped};
  std::uint64_t latest_requested_version{0};
  std::uint64_t latest_completed_version{0};
  std::uint64_t latest_published_version{0};
  std::uint64_t latest_result_version{0};
  PlaybackCommandKind latest_command{PlaybackCommandKind::Stop};
  PlaybackCommandStatus latest_status{PlaybackCommandStatus::Succeeded};
  std::optional<PlaybackControlError> latest_error{};
  bool shutting_down{false};
  PlaybackDiagnostics playback{};
};

// Non-blocking GUI-facing control facade. Requests only append to a bounded
// queue; a dedicated thread serializes the synchronous playback controller.
// Versions strictly increase. A completion is published as effective state
// only when it still matches the newest accepted intent.
class AsyncRealtimeAudioPlayback final {
public:
  explicit AsyncRealtimeAudioPlayback(std::shared_ptr<PlaybackAudioProvider> provider,
                                      RealtimePlaybackConfiguration playback_configuration = {},
                                      std::unique_ptr<AudioOutputDevice> device = nullptr,
                                      AsyncRealtimePlaybackConfiguration async_configuration = {});
  ~AsyncRealtimeAudioPlayback();

  AsyncRealtimeAudioPlayback(const AsyncRealtimeAudioPlayback&) = delete;
  AsyncRealtimeAudioPlayback& operator=(const AsyncRealtimeAudioPlayback&) = delete;

  [[nodiscard]] PlaybackCommandReceipt request_start(std::int64_t start_sample = 0) noexcept;
  [[nodiscard]] PlaybackCommandReceipt request_pause() noexcept;
  [[nodiscard]] PlaybackCommandReceipt request_resume() noexcept;
  [[nodiscard]] PlaybackCommandReceipt request_seek(std::int64_t sample) noexcept;
  // Stop clears older queued commands and cooperatively cancels an active
  // provider/prefill operation before returning the receipt.
  [[nodiscard]] PlaybackCommandReceipt request_stop() noexcept;

  [[nodiscard]] PlaybackState requested_state() const noexcept;
  [[nodiscard]] PlaybackState effective_state() const noexcept;
  [[nodiscard]] std::uint64_t latest_requested_version() const noexcept;
  [[nodiscard]] std::uint64_t latest_completed_version() const noexcept;
  [[nodiscard]] std::int64_t sample_counter() const noexcept;
  [[nodiscard]] std::int64_t submitted_sample_counter() const noexcept;
  [[nodiscard]] bool device_present() const noexcept;
  [[nodiscard]] bool device_open() const noexcept;
  [[nodiscard]] std::size_t render_callback(std::span<float> interleaved_output) noexcept;
  [[nodiscard]] AsyncPlaybackDiagnostics diagnostics() const;
  // Read the latest meter levels and reset the accumulators. Safe to call
  // from the GUI thread.
  [[nodiscard]] PlaybackMeter::Reading read_meter() const noexcept;
  [[nodiscard]] RealtimeLoudnessAnalyzer::Reading read_loudness() const noexcept;

  // Test/worker synchronization helper. GUI code should observe versions via
  // diagnostics instead of waiting.
  [[nodiscard]] bool wait_until_completed(std::uint64_t version,
                                          std::chrono::milliseconds timeout) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio

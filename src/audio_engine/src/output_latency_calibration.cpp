// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/output_latency_calibration.h"

#include "video_editor/audio_engine/realtime_playback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace video_editor::audio {
namespace {

struct MeasurementState final {
  std::atomic<std::uint64_t> callback_count{0};
};

void measurement_callback(void* user_data, float* output,
                          const std::size_t frame_count) noexcept {
  auto* state = static_cast<MeasurementState*>(user_data);
  if (state == nullptr || output == nullptr) {
    return;
  }
  state->callback_count.fetch_add(1U, std::memory_order_relaxed);
  const std::size_t sample_count = frame_count * kPlaybackAudioFormat.channels;
  std::fill(output, output + sample_count, 0.0F);
}

} // namespace

std::optional<std::uint64_t>
measure_output_latency_frames(AudioOutputDevice& device,
                            const OutputLatencyMeasurementOptions& options) {
  if (options.minimum_callbacks == 0U || options.timeout.count() <= 0 ||
      device.is_open()) {
    return std::nullopt;
  }

  MeasurementState state{};
  const AudioDeviceResult opened =
      device.open({.format = kPlaybackAudioFormat,
                   .callback = &measurement_callback,
                   .user_data = &state,
                   .device_id = options.device_id});
  if (!opened) {
    return std::nullopt;
  }

  const AudioDeviceResult started = device.start();
  if (!started) {
    device.close();
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  while (state.callback_count.load(std::memory_order_acquire) < options.minimum_callbacks &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const std::optional<std::uint64_t> measured =
      state.callback_count.load(std::memory_order_acquire) >= options.minimum_callbacks
          ? std::optional<std::uint64_t>{device.estimated_output_latency_frames()}
          : std::nullopt;

  device.stop();
  device.close();
  return measured;
}

} // namespace video_editor::audio

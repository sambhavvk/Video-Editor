// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/audio_output_device.h"
#include "video_editor/audio_engine/realtime_playback.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace video_editor::audio {
namespace {

class FakeAudioDevice final : public AudioOutputDevice {
public:
  AudioDeviceResult open(const AudioDeviceConfiguration& configuration) override {
    if (configuration.callback == nullptr) {
      return AudioDeviceResult::failure(AudioDeviceErrorCode::InvalidConfiguration,
                                        "test callback is missing");
    }
    configuration_ = configuration;
    open_.store(true, std::memory_order_release);
    return AudioDeviceResult::success();
  }

  AudioDeviceResult start() override {
    if (!open_.load(std::memory_order_acquire)) {
      return AudioDeviceResult::failure(AudioDeviceErrorCode::StartFailed, "test device is closed");
    }
    running_.store(true, std::memory_order_release);
    return AudioDeviceResult::success();
  }

  void stop() noexcept override {
    running_.store(false, std::memory_order_release);
  }

  void close() noexcept override {
    stop();
    open_.store(false, std::memory_order_release);
    configuration_ = {};
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool is_running() const noexcept override {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept override {
    return 0U;
  }

  [[nodiscard]] bool pump(const std::span<float> output) noexcept {
    if (!running_.load(std::memory_order_acquire) || configuration_.callback == nullptr ||
        output.size() % configuration_.format.channels != 0U) {
      return false;
    }
    configuration_.callback(configuration_.user_data, output.data(),
                            output.size() / configuration_.format.channels);
    return true;
  }

private:
  AudioDeviceConfiguration configuration_{};
  std::atomic<bool> open_{false};
  std::atomic<bool> running_{false};
};

class SineWaveProvider final : public PlaybackAudioProvider {
public:
  PlaybackRenderResult render(const PlaybackRenderRequest& request) override {
    if (request.cancellation.stop_requested()) {
      return PlaybackRenderResult::cancelled("long playback test cancelled");
    }

    AudioBlock block(kPlaybackAudioFormat, request.start_sample, request.sample_count);
    constexpr double frequency = 440.0;
    constexpr double sample_rate = 48'000.0;
    constexpr float amplitude = 0.25F;
    const auto left = block.channel(0);
    const auto right = block.channel(1);
    for (std::size_t frame = 0; frame < request.sample_count; ++frame) {
      const double sample = static_cast<double>(request.start_sample) + static_cast<double>(frame);
      const float value =
          amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency *
                                                  sample / sample_rate));
      left[frame] = value;
      right[frame] = value;
    }
    return PlaybackRenderResult::ready(std::move(block));
  }
};

[[nodiscard]] bool long_tests_are_enabled() {
  return std::getenv("VE_RUN_LONG_TESTS") != nullptr;
}

void wait_for_audio_frames(const RealtimeAudioPlayback& playback, const std::size_t frames) {
  // The callback is deliberately accelerated beyond wall-clock time. Yield only
  // while the worker refills the bounded ring; never sleep or add test latency.
  constexpr std::size_t maximum_yields = 100'000;
  for (std::size_t attempt = 0; playback.diagnostics().queued_frames < frames; ++attempt) {
    if (attempt == maximum_yields) {
      ADD_FAILURE() << "audio pre-render worker did not keep the ring filled";
      return;
    }
    std::this_thread::yield();
  }
}

TEST(XrunValidation, OneHourPlaybackHasZeroXrunsWithBoundedJitter) {
  if (!long_tests_are_enabled()) {
    GTEST_SKIP() << "Set VE_RUN_LONG_TESTS=1 to run the one-hour xrun validation";
  }

  constexpr std::size_t callback_count = 180'000;
  auto provider = std::make_shared<SineWaveProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider,
                                 {.ring_capacity_frames = 96'000,
                                  .render_block_frames = 960,
                                  .prefill_frames = 4'800,
                                  .prefill_timeout = std::chrono::seconds(5)},
                                 std::move(device));
  ASSERT_TRUE(playback.start(0));

  std::vector<float> output;
  for (std::size_t callback = 0; callback < callback_count; ++callback) {
    const std::size_t frame_count = callback % 2U == 0U ? 864U : 1'056U;
    wait_for_audio_frames(playback, frame_count);
    output.assign(frame_count * kPlaybackAudioFormat.channels, 0.0F);
    ASSERT_TRUE(fake->pump(output));
  }

  const PlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.xrun_count, 0U);
  EXPECT_EQ(diagnostics.underrun_frames, 0U);
}

TEST(DriftValidation, TwoHourPlaybackDriftsLessThanOneFrame) {
  if (!long_tests_are_enabled()) {
    GTEST_SKIP() << "Set VE_RUN_LONG_TESTS=1 to run the two-hour A/V drift validation";
  }

  constexpr std::size_t callback_count = 360'000;
  constexpr std::size_t frame_count = 960;
  auto provider = std::make_shared<SineWaveProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider,
                                 {.ring_capacity_frames = 96'000,
                                  .render_block_frames = frame_count,
                                  .prefill_frames = 4'800,
                                  .prefill_timeout = std::chrono::seconds(5)},
                                 std::move(device));
  ASSERT_TRUE(playback.start(0));

  // sample_counter() is latency-compensated by the current submitted device
  // buffer. Start the video clock at that same phase, then advance it exactly
  // once per callback from the shared 48 kHz master.
  std::int64_t video_clock = -static_cast<std::int64_t>(frame_count);
  std::vector<float> output(frame_count * kPlaybackAudioFormat.channels);
  for (std::size_t callback = 0; callback < callback_count; ++callback) {
    wait_for_audio_frames(playback, frame_count);
    ASSERT_TRUE(fake->pump(output));
    video_clock += static_cast<std::int64_t>(frame_count);
  }

  const std::int64_t drift = playback.sample_counter() - video_clock;
  EXPECT_LT(std::llabs(drift), 1);
}

} // namespace
} // namespace video_editor::audio

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/async_realtime_playback.h"
#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/audio_device_manager.h"
#include "video_editor/audio_engine/dsp.h"
#include "video_editor/audio_engine/loudness_meter.h"
#include "video_editor/audio_engine/miniaudio_output_device.h"
#include "video_editor/audio_engine/realtime_playback.h"
#include "video_editor/audio_engine/spsc_audio_ring.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
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
    ++open_count;
    return AudioDeviceResult::success();
  }

  AudioDeviceResult start() override {
    if (!open_.load(std::memory_order_acquire)) {
      return AudioDeviceResult::failure(AudioDeviceErrorCode::StartFailed, "test device is closed");
    }
    running_.store(true, std::memory_order_release);
    ++start_count;
    return AudioDeviceResult::success();
  }

  void stop() noexcept override {
    if (running_.load(std::memory_order_acquire)) {
      ++stop_count;
    }
    running_.store(false, std::memory_order_release);
  }

  void close() noexcept override {
    stop();
    if (open_.load(std::memory_order_acquire)) {
      ++close_count;
    }
    open_.store(false, std::memory_order_release);
    estimated_latency_frames_.store(0U, std::memory_order_release);
    configuration_ = {};
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool is_running() const noexcept override {
    return running_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept override {
    return estimated_latency_frames_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool pump(const std::span<float> output) noexcept {
    if (!running_.load(std::memory_order_acquire) || configuration_.callback == nullptr ||
        output.size() % configuration_.format.channels != 0U) {
      return false;
    }
    estimated_latency_frames_.store(output.size() / configuration_.format.channels,
                                    std::memory_order_release);
    configuration_.callback(configuration_.user_data, output.data(),
                            output.size() / configuration_.format.channels);
    return true;
  }

  AudioDeviceConfiguration configuration_{};
  std::atomic<bool> open_{false};
  std::atomic<bool> running_{false};
  int open_count{0};
  int start_count{0};
  int stop_count{0};
  int close_count{0};
  std::atomic<std::uint64_t> estimated_latency_frames_{0};
};

// Coordinates a diagnostics query with start() so the query captures the
// device's old running value while the async command is able to complete. This
// deterministically exercises publication consistency without entering the
// realtime callback path.
class StartDiagnosticsRaceDevice final : public AudioOutputDevice {
public:
  AudioDeviceResult open(const AudioDeviceConfiguration& configuration) override {
    configuration_ = configuration;
    open_.store(true, std::memory_order_release);
    return AudioDeviceResult::success();
  }

  AudioDeviceResult start() override {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return running_query_entered_; });
    running_.store(true, std::memory_order_release);
    changed_.notify_all();
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
    if (running_.load(std::memory_order_acquire)) {
      return true;
    }
    std::unique_lock lock(mutex_);
    running_query_entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return running_.load(std::memory_order_acquire); });
    lock.unlock();
    // Give the control thread time to publish after start() returns. With a
    // split transport/result snapshot this exposed Succeeded + false.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return false;
  }

  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept override {
    return 0U;
  }

private:
  AudioDeviceConfiguration configuration_{};
  std::atomic<bool> open_{false};
  mutable std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mutable bool running_query_entered_{false};
};

struct ProviderRequestRecord final {
  std::int64_t start_sample{0};
  std::size_t sample_count{0};
  std::uint64_t epoch{0};
};

[[nodiscard]] float deterministic_sample(const std::int64_t sample) {
  return static_cast<float>(sample % 1'000) / 1'000.0F;
}

class DeterministicPlaybackProvider : public PlaybackAudioProvider {
public:
  explicit DeterministicPlaybackProvider(std::optional<std::int64_t> end_sample = std::nullopt)
      : callback_thread_(std::this_thread::get_id()), end_sample_(end_sample) {}

  PlaybackRenderResult render(const PlaybackRenderRequest& request) override {
    if (std::this_thread::get_id() == callback_thread_) {
      rendered_on_callback_thread.store(true, std::memory_order_relaxed);
    }
    {
      std::lock_guard lock(mutex_);
      requests_.push_back({.start_sample = request.start_sample,
                           .sample_count = request.sample_count,
                           .epoch = request.epoch});
    }
    render_calls.fetch_add(1U, std::memory_order_relaxed);
    if (request.cancellation.stop_requested()) {
      return PlaybackRenderResult::cancelled("test provider cancelled");
    }
    if (end_sample_.has_value() && request.start_sample >= *end_sample_) {
      return PlaybackRenderResult::end_of_stream();
    }
    AudioBlock block(kPlaybackAudioFormat, request.start_sample, request.sample_count);
    for (std::size_t index = 0; index < request.sample_count; ++index) {
      const float value =
          deterministic_sample(request.start_sample + static_cast<std::int64_t>(index));
      block.channel(0)[index] = value;
      block.channel(1)[index] = -value;
    }
    return PlaybackRenderResult::ready(std::move(block));
  }

  [[nodiscard]] std::vector<ProviderRequestRecord> requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  std::atomic<std::uint64_t> render_calls{0};
  std::atomic<bool> rendered_on_callback_thread{false};

private:
  std::thread::id callback_thread_;
  std::optional<std::int64_t> end_sample_;
  mutable std::mutex mutex_;
  std::vector<ProviderRequestRecord> requests_;
};

class BlockingPlaybackProvider final : public DeterministicPlaybackProvider {
public:
  PlaybackRenderResult render(const PlaybackRenderRequest& request) override {
    const std::uint64_t call = calls_.fetch_add(1U, std::memory_order_relaxed);
    if (call > 0U) {
      std::unique_lock lock(block_mutex_);
      blocked_ = true;
      blocked_changed_.notify_all();
      blocked_changed_.wait(lock, request.cancellation, [this] { return released_; });
      if (request.cancellation.stop_requested()) {
        return PlaybackRenderResult::cancelled("blocking provider stopped");
      }
    }
    return DeterministicPlaybackProvider::render(request);
  }

  [[nodiscard]] bool wait_until_blocked() {
    std::unique_lock lock(block_mutex_);
    return blocked_changed_.wait_for(lock, std::chrono::seconds(1), [this] { return blocked_; });
  }

  void release() {
    {
      std::lock_guard lock(block_mutex_);
      released_ = true;
    }
    blocked_changed_.notify_all();
  }

private:
  std::atomic<std::uint64_t> calls_{0};
  std::mutex block_mutex_;
  std::condition_variable_any blocked_changed_;
  bool blocked_{false};
  bool released_{false};
};

class ArmableBlockingPlaybackProvider final : public DeterministicPlaybackProvider {
public:
  PlaybackRenderResult render(const PlaybackRenderRequest& request) override {
    if (armed_.load(std::memory_order_acquire)) {
      std::unique_lock lock(block_mutex_);
      blocked_ = true;
      render_thread_ = std::this_thread::get_id();
      blocked_changed_.notify_all();
      blocked_changed_.wait(lock, request.cancellation, [this] { return released_; });
      if (request.cancellation.stop_requested()) {
        cancellations.fetch_add(1U, std::memory_order_relaxed);
        return PlaybackRenderResult::cancelled("armable provider stopped");
      }
    }
    return DeterministicPlaybackProvider::render(request);
  }

  void arm() {
    std::lock_guard lock(block_mutex_);
    blocked_ = false;
    released_ = false;
    render_thread_ = {};
    armed_.store(true, std::memory_order_release);
  }

  [[nodiscard]] bool wait_until_blocked() {
    std::unique_lock lock(block_mutex_);
    return blocked_changed_.wait_for(lock, std::chrono::seconds(1), [this] { return blocked_; });
  }

  [[nodiscard]] std::thread::id render_thread() const {
    std::lock_guard lock(block_mutex_);
    return render_thread_;
  }

  void release() {
    {
      std::lock_guard lock(block_mutex_);
      released_ = true;
    }
    blocked_changed_.notify_all();
  }

  std::atomic<std::uint64_t> cancellations{0};

private:
  std::atomic<bool> armed_{false};
  mutable std::mutex block_mutex_;
  std::condition_variable_any blocked_changed_;
  bool blocked_{false};
  bool released_{false};
  std::thread::id render_thread_{};
};

class FailingPlaybackProvider final : public PlaybackAudioProvider {
public:
  PlaybackRenderResult render(const PlaybackRenderRequest&) override {
    return PlaybackRenderResult::failure("intentional provider failure");
  }
};

class RecoverablePlaybackProvider final : public DeterministicPlaybackProvider {
public:
  PlaybackRenderResult render(const PlaybackRenderRequest& request) override {
    if (failing.load(std::memory_order_acquire)) {
      return PlaybackRenderResult::failure("temporary provider failure");
    }
    return DeterministicPlaybackProvider::render(request);
  }

  std::atomic<bool> failing{true};
};

[[nodiscard]] constexpr RealtimePlaybackConfiguration small_configuration() {
  return {.ring_capacity_frames = 16,
          .render_block_frames = 4,
          .prefill_frames = 8,
          .prefill_timeout = std::chrono::seconds(1),
          .device_id = {}};
}

TEST(AudioBlock, UsesPlanarStorageAndExactStartSample) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 960, 3);
  block.channel(0)[0] = 1.0F;
  block.channel(1)[0] = -1.0F;
  const auto interleaved = block.interleaved();
  EXPECT_EQ(block.start_sample(), 960);
  ASSERT_EQ(interleaved.size(), 6U);
  EXPECT_FLOAT_EQ(interleaved[0], 1.0F);
  EXPECT_FLOAT_EQ(interleaved[1], -1.0F);
}

TEST(RealtimeLoudnessMeter, SupportsConcurrentPollingWithoutAllocating) {
  RealtimeLoudnessMeter meter;
  std::array<float, 2 * 480> samples{};
  std::fill(samples.begin(), samples.end(), 0.25F);
  meter.process(samples.data(), 480U, 2U);
  const auto reading = meter.read();
  ASSERT_EQ(reading.sample_count, 480U);
  EXPECT_EQ(reading.channels, 2U);
  EXPECT_NEAR(reading.sample_peak, 0.25F, 1.0e-6F);
  EXPECT_NEAR(reading.integrated_lufs, 20.0 * std::log10(0.25) - 0.691, 0.001);
  EXPECT_EQ(meter.read().sample_count, 0U);
}

TEST(RealtimeLoudnessAnalyzer, PublishesVersionedEbuR128WindowsOffCallbackQueue) {
  constexpr AudioFormat format{.sample_rate = 48'000, .channels = 2};
  RealtimeLoudnessAnalyzer analyzer(format, {.queue_blocks = 8, .maximum_block_frames = 4'800});
  std::array<float, 4'800U * 2U> samples{};
  std::fill(samples.begin(), samples.end(), 0.25F);

  for (int block = 0; block < 40; ++block) {
    ASSERT_TRUE(analyzer.submit(samples.data(), 4'800U, format.channels));
    for (int attempt = 0; attempt < 100 && analyzer.read().stale; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  for (int attempt = 0; attempt < 200 && analyzer.read().stale; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto reading = analyzer.read();
  EXPECT_GT(reading.version, 0U);
  EXPECT_TRUE(reading.momentary_valid);
  EXPECT_TRUE(reading.short_term_valid);
  EXPECT_TRUE(reading.integrated_valid);
  EXPECT_FALSE(reading.stale);
  EXPECT_EQ(reading.dropped_blocks, 0U);
  EXPECT_EQ(reading.channels, format.channels);
  AudioBlock reference_block(format, 0, 40U * 4'800U);
  for (std::size_t frame = 0; frame < reference_block.frame_count(); ++frame) {
    reference_block.channel(0)[frame] = 0.25F;
    reference_block.channel(1)[frame] = 0.25F;
  }
  LoudnessMeter reference_meter(format);
  ASSERT_TRUE(reference_meter.add(reference_block));
  const auto reference = reference_meter.reading();
  ASSERT_TRUE(reference.integrated_lufs.has_value());
  EXPECT_NEAR(reading.integrated_lufs, *reference.integrated_lufs, 0.001);
}

TEST(RealtimeLoudnessAnalyzer, DropsWhenBoundedQueueIsOverloadedWithoutBlockingProducer) {
  RealtimeLoudnessAnalyzer analyzer({.sample_rate = 48'000, .channels = 2},
                                    {.queue_blocks = 2, .maximum_block_frames = 8});
  std::array<float, 16> samples{};
  bool rejected = false;
  for (int block = 0; block < 100'000; ++block) {
    rejected = !analyzer.submit(samples.data(), 8U, 2U) || rejected;
  }
  EXPECT_TRUE(rejected);
  for (int attempt = 0; attempt < 100 && analyzer.read().stale; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GT(analyzer.read().dropped_blocks, 0U);
}

TEST(RealtimeLoudnessAnalyzer, ResetDiscardsQueuedOldGenerationBeforeNewAudio) {
  RealtimeLoudnessAnalyzer analyzer({.sample_rate = 48'000, .channels = 2},
                                    {.queue_blocks = 8, .maximum_block_frames = 480});
  std::array<float, 960> samples{};
  std::fill(samples.begin(), samples.end(), 0.25F);
  ASSERT_TRUE(analyzer.submit(samples.data(), 480U, 2U));
  analyzer.reset();
  const auto after_reset = analyzer.read();
  EXPECT_EQ(after_reset.version, 0U);
  EXPECT_FALSE(after_reset.integrated_valid);
  EXPECT_TRUE(after_reset.stale);

  std::fill(samples.begin(), samples.end(), 0.5F);
  ASSERT_TRUE(analyzer.submit(samples.data(), 480U, 2U));
  for (int attempt = 0; attempt < 200 && analyzer.read().stale; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto reading = analyzer.read();
  EXPECT_EQ(reading.dropped_blocks, 0U);
  EXPECT_GT(reading.version, 0U);
  EXPECT_EQ(reading.analyzed_frames, 480U);
  EXPECT_FALSE(reading.stale);
}

TEST(RealtimeLoudnessAnalyzer, FullQueueShutdownIsBounded) {
  const auto begin = std::chrono::steady_clock::now();
  {
    RealtimeLoudnessAnalyzer analyzer({.sample_rate = 48'000, .channels = 2},
                                      {.queue_blocks = 2, .maximum_block_frames = 2'048});
    std::array<float, 4'096> samples{};
    for (int block = 0; block < 32; ++block) {
      static_cast<void>(analyzer.submit(samples.data(), 2'048U, 2U));
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST(AudioDeviceRecovery, ReopensSelectedDeviceAfterHotPlug) {
  FakeAudioDevice fake;
  const AudioDeviceConfiguration configuration{
      .format = kPlaybackAudioFormat,
      .callback =
          [](void* user_data, float* destination, std::size_t frames) noexcept {
            static_cast<void>(user_data);
            std::fill(destination, destination + (frames * 2U), 0.0F);
          },
      .user_data = nullptr,
      .device_id = "usb"};
  AudioDeviceRecovery recovery(fake);
  const std::array<AudioDeviceInfo, 1> connected{
      AudioDeviceInfo{.id = "usb", .name = "USB", .is_default = true, .connected = true}};
  ASSERT_EQ(recovery.refresh(connected).code, AudioDeviceRecoveryCode::NoChange);
  ASSERT_EQ(recovery.select("usb", configuration).code, AudioDeviceRecoveryCode::Reopened);
  ASSERT_TRUE(fake.is_open());
  EXPECT_EQ(fake.configuration_.device_id, "usb");
  ASSERT_TRUE(fake.start());
  ASSERT_TRUE(fake.is_running());

  const std::array<AudioDeviceInfo, 1> removed{
      AudioDeviceInfo{.id = "usb", .name = "USB", .is_default = true, .connected = false}};
  EXPECT_EQ(recovery.refresh(removed).code, AudioDeviceRecoveryCode::Disconnected);
  EXPECT_FALSE(fake.is_open());
  EXPECT_FALSE(fake.is_running());

  EXPECT_EQ(recovery.refresh(connected).code, AudioDeviceRecoveryCode::Reopened);
  EXPECT_TRUE(fake.is_open());
  // The coordinator remembers that the device was running and restarts it.
  EXPECT_TRUE(fake.is_running());
}

TEST(AudioDeviceRecovery, ReopensWhenBackendDefaultChanges) {
  FakeAudioDevice fake;
  const AudioDeviceConfiguration configuration{
      .format = kPlaybackAudioFormat,
      .callback =
          [](void*, float* destination, std::size_t frames) noexcept {
            std::fill(destination, destination + (frames * 2U), 0.0F);
          },
      .user_data = nullptr,
      .device_id = {}};
  AudioDeviceRecovery recovery(fake);
  const std::array<AudioDeviceInfo, 2> first{
      AudioDeviceInfo{.id = "a", .name = "A", .is_default = true, .connected = true},
      AudioDeviceInfo{.id = "b", .name = "B", .is_default = false, .connected = true}};
  ASSERT_EQ(recovery.refresh(first).code, AudioDeviceRecoveryCode::NoChange);
  ASSERT_EQ(recovery.select({}, configuration).code, AudioDeviceRecoveryCode::Reopened);
  ASSERT_TRUE(fake.start());
  const std::array<AudioDeviceInfo, 2> changed{
      AudioDeviceInfo{.id = "a", .name = "A", .is_default = false, .connected = true},
      AudioDeviceInfo{.id = "b", .name = "B", .is_default = true, .connected = true}};
  EXPECT_EQ(recovery.refresh(changed).code, AudioDeviceRecoveryCode::Reopened);
  EXPECT_TRUE(fake.is_running());
  EXPECT_EQ(fake.configuration_.device_id, "");
}

TEST(SpscAudioRing, PreservesOrderAcrossWrapWithoutAllocation) {
  SpscAudioRing ring(3, 2);
  const std::array<float, 6> first{1, 2, 3, 4, 5, 6};
  EXPECT_EQ(ring.write(first), 3U);
  std::array<float, 4> partial{};
  EXPECT_EQ(ring.read(partial), 2U);
  EXPECT_EQ(partial, (std::array<float, 4>{1, 2, 3, 4}));

  const std::array<float, 4> second{7, 8, 9, 10};
  EXPECT_EQ(ring.write(second), 2U);
  std::array<float, 6> remainder{};
  EXPECT_EQ(ring.read(remainder), 3U);
  EXPECT_EQ(remainder, (std::array<float, 6>{5, 6, 7, 8, 9, 10}));
}

TEST(RealtimeAudioPlayback, DeviceFramesDriveTheExactFortyEightKilohertzMasterClock) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider,
                                 {.ring_capacity_frames = 1'920,
                                  .render_block_frames = 480,
                                  .prefill_frames = 960,
                                  .prefill_timeout = std::chrono::seconds(1),
                                  .device_id = {}},
                                 std::move(device));

  ASSERT_TRUE(playback.start(48'001));
  EXPECT_TRUE(playback.device_present());
  EXPECT_TRUE(playback.device_open());
  EXPECT_EQ(fake->configuration_.format.sample_rate, 48'000U);
  EXPECT_EQ(fake->configuration_.format.channels, 2U);
  EXPECT_EQ(playback.sample_counter(), 48'001);

  std::array<float, 960> first{};
  ASSERT_TRUE(fake->pump(first));
  EXPECT_FLOAT_EQ(first[0], deterministic_sample(48'001));
  EXPECT_FLOAT_EQ(first[1], -deterministic_sample(48'001));
  EXPECT_EQ(playback.sample_counter(), 48'001);
  EXPECT_EQ(playback.submitted_sample_counter(), 48'481);
  EXPECT_NEAR(playback.clock_seconds(), 48'001.0 / 48'000.0, 1.0e-12);
  const PlaybackDiagnostics first_diagnostics = playback.diagnostics();
  EXPECT_EQ(first_diagnostics.playback_position_sample, 48'001);
  EXPECT_EQ(first_diagnostics.submitted_sample_counter, 48'481);
  EXPECT_EQ(first_diagnostics.estimated_output_latency_frames, 480U);
  EXPECT_EQ(first_diagnostics.clock_uncertainty_frames, 480U);
  EXPECT_TRUE(first_diagnostics.clock_is_estimated);

  std::array<float, 960> second{};
  ASSERT_TRUE(fake->pump(second));
  EXPECT_EQ(playback.sample_counter(), 48'481);
  EXPECT_EQ(playback.submitted_sample_counter(), 48'961);
  EXPECT_LT(playback.sample_counter(), playback.submitted_sample_counter());
  EXPECT_FALSE(provider->rendered_on_callback_thread.load(std::memory_order_relaxed));

  const std::uint64_t playing_epoch = playback.epoch();
  playback.stop();
  EXPECT_EQ(playback.state(), PlaybackState::Stopped);
  EXPECT_EQ(playback.sample_counter(), 0);
  EXPECT_GT(playback.epoch(), playing_epoch);
  EXPECT_FALSE(fake->is_open());
}

TEST(RealtimeAudioPlayback, CalibratedLatencyFramesDriveTheMasterClockWithReducedUncertainty) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(
      provider,
      {.ring_capacity_frames = 1'920,
       .render_block_frames = 480,
       .prefill_frames = 960,
       .prefill_timeout = std::chrono::seconds(1),
       .device_id = {},
       .calibrated_latency_frames = 960U},
      std::move(device));

  ASSERT_TRUE(playback.start(48'001));
  EXPECT_EQ(playback.sample_counter(), 48'001);

  std::array<float, 960> first{};
  ASSERT_TRUE(fake->pump(first));
  EXPECT_EQ(playback.submitted_sample_counter(), 48'481);
  EXPECT_EQ(playback.sample_counter(), 48'001);

  std::array<float, 960> second{};
  ASSERT_TRUE(fake->pump(second));
  EXPECT_EQ(playback.submitted_sample_counter(), 48'961);
  EXPECT_EQ(playback.sample_counter(), 48'001);

  const PlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_FALSE(diagnostics.clock_is_estimated);
  EXPECT_EQ(diagnostics.estimated_output_latency_frames, 480U);
  EXPECT_LT(diagnostics.clock_uncertainty_frames, 960U);
  EXPECT_EQ(diagnostics.clock_uncertainty_frames, 240U);
  playback.stop();
}

TEST(RealtimeAudioPlayback, PublicManualCallbackCannotBecomeASecondPhysicalDeviceConsumer) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));
  ASSERT_TRUE(playback.start(301));

  std::array<float, 8> manual_output;
  manual_output.fill(1.0F);
  EXPECT_EQ(playback.render_callback(manual_output), 0U);
  EXPECT_EQ(playback.sample_counter(), 301);
  EXPECT_TRUE(std::ranges::all_of(manual_output, [](const float value) { return value == 0.0F; }));

  std::array<float, 8> device_output{};
  ASSERT_TRUE(fake->pump(device_output));
  EXPECT_FLOAT_EQ(device_output[0], deterministic_sample(301));
  EXPECT_EQ(playback.sample_counter(), 301);
  EXPECT_EQ(playback.submitted_sample_counter(), 305);
}

TEST(RealtimeAudioPlayback, PauseFreezesAndResumeContinuesTheExistingClockAndQueue) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));
  ASSERT_TRUE(playback.start(10));

  std::array<float, 8> before_pause{};
  ASSERT_TRUE(fake->pump(before_pause));
  EXPECT_EQ(playback.sample_counter(), 10);
  EXPECT_EQ(playback.submitted_sample_counter(), 14);
  ASSERT_TRUE(playback.pause());
  EXPECT_EQ(playback.state(), PlaybackState::Paused);
  EXPECT_FALSE(fake->is_running());

  std::array<float, 8> paused_output;
  paused_output.fill(1.0F);
  EXPECT_EQ(playback.render_callback(paused_output), 0U);
  EXPECT_EQ(playback.sample_counter(), 10);
  EXPECT_TRUE(std::ranges::all_of(paused_output, [](const float value) { return value == 0.0F; }));

  ASSERT_TRUE(playback.resume());
  std::array<float, 8> after_resume{};
  ASSERT_TRUE(fake->pump(after_resume));
  EXPECT_FLOAT_EQ(after_resume[0], deterministic_sample(14));
  EXPECT_EQ(playback.sample_counter(), 14);
  EXPECT_EQ(playback.submitted_sample_counter(), 18);
  EXPECT_EQ(fake->start_count, 2);
}

TEST(RealtimeAudioPlayback, SeekChangesEpochAndCannotPlayQueuedSamplesFromTheOldPosition) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));
  ASSERT_TRUE(playback.start(101));
  std::array<float, 8> before_seek{};
  ASSERT_TRUE(fake->pump(before_seek));
  const std::uint64_t old_epoch = playback.epoch();

  ASSERT_TRUE(playback.seek(10'001));
  EXPECT_GT(playback.epoch(), old_epoch);
  EXPECT_EQ(playback.sample_counter(), 10'001);
  std::array<float, 8> after_seek{};
  ASSERT_TRUE(fake->pump(after_seek));
  EXPECT_FLOAT_EQ(after_seek[0], deterministic_sample(10'001));
  EXPECT_EQ(playback.sample_counter(), 10'001);
  EXPECT_EQ(playback.submitted_sample_counter(), 10'005);

  const auto requests = provider->requests();
  EXPECT_TRUE(
      std::ranges::any_of(requests, [current_epoch = playback.epoch()](const auto& request) {
        return request.epoch == current_epoch && request.start_sample == 10'001;
      }));
}

TEST(RealtimeAudioPlayback, CallbackDoesNotWaitForProviderAndAccountsExactUnderrunSilence) {
  auto provider = std::make_shared<BlockingPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider,
                                 {.ring_capacity_frames = 8,
                                  .render_block_frames = 4,
                                  .prefill_frames = 4,
                                  .prefill_timeout = std::chrono::seconds(1),
                                  .device_id = {}},
                                 std::move(device));
  ASSERT_TRUE(playback.start(0));
  ASSERT_TRUE(provider->wait_until_blocked());

  std::array<float, 16> output;
  output.fill(1.0F);
  ASSERT_TRUE(fake->pump(output));
  EXPECT_FLOAT_EQ(output[0], 0.0F);
  EXPECT_FLOAT_EQ(output[2], deterministic_sample(1));
  for (std::size_t index = 8; index < output.size(); ++index) {
    EXPECT_FLOAT_EQ(output[index], 0.0F);
  }
  EXPECT_EQ(playback.sample_counter(), 0);
  EXPECT_EQ(playback.submitted_sample_counter(), 8);
  const PlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.xrun_count, 1U);
  EXPECT_EQ(diagnostics.underrun_frames, 4U);
  EXPECT_EQ(diagnostics.callback_count, 1U);
  EXPECT_FALSE(provider->rendered_on_callback_thread.load(std::memory_order_relaxed));

  provider->release();
}

TEST(RealtimeAudioPlayback, EndOfStreamStopsProviderDemandButDeviceTimeStillAdvances) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>(4);
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  RealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));
  ASSERT_TRUE(playback.start(0));
  EXPECT_TRUE(playback.diagnostics().provider_exhausted);
  const std::uint64_t calls_at_end = provider->render_calls.load(std::memory_order_relaxed);

  std::array<float, 16> output{};
  ASSERT_TRUE(fake->pump(output));
  EXPECT_EQ(playback.sample_counter(), 0);
  EXPECT_EQ(playback.submitted_sample_counter(), 8);
  EXPECT_EQ(playback.diagnostics().underrun_frames, 4U);
  EXPECT_EQ(provider->render_calls.load(std::memory_order_relaxed), calls_at_end);
}

TEST(RealtimeAudioPlayback, NoDeviceBuildSupportsManualSoftwareCallbackFallback) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  RealtimeAudioPlayback playback(provider, small_configuration());
  ASSERT_TRUE(playback.start(2'001));
  EXPECT_FALSE(playback.device_present());
  EXPECT_FALSE(playback.device_open());

  std::array<float, 8> output{};
  EXPECT_EQ(playback.render_callback(output), 4U);
  EXPECT_FLOAT_EQ(output[0], deterministic_sample(2'001));
  EXPECT_EQ(playback.sample_counter(), 2'001);
  EXPECT_EQ(playback.submitted_sample_counter(), 2'005);
}

TEST(RealtimeAudioPlayback, ConcurrentManualConsumersCannotRaceSeekRingReset) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  constexpr std::size_t ring_capacity = 64;
  RealtimeAudioPlayback playback(provider, {.ring_capacity_frames = ring_capacity,
                                            .render_block_frames = 8,
                                            .prefill_frames = 16,
                                            .prefill_timeout = std::chrono::seconds(1),
                                            .device_id = {}});
  ASSERT_TRUE(playback.start(0));

  const auto consume = [&playback](const std::stop_token stop) {
    std::array<float, 32> output{};
    while (!stop.stop_requested()) {
      static_cast<void>(playback.render_callback(output));
      std::this_thread::yield();
    }
  };
  std::jthread first_consumer(consume);
  std::jthread second_consumer(consume);

  for (std::int64_t index = 1; index <= 100; ++index) {
    ASSERT_TRUE(playback.seek(index * 100));
    EXPECT_LE(playback.diagnostics().queued_frames, ring_capacity);
  }

  first_consumer.request_stop();
  second_consumer.request_stop();
  first_consumer.join();
  second_consumer.join();
  playback.stop();
  EXPECT_EQ(playback.state(), PlaybackState::Stopped);
  EXPECT_EQ(playback.diagnostics().queued_frames, 0U);
}

TEST(RealtimeAudioPlayback, ConfigurationSupportsBoundedLargeDecodeAheadBlocks) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  EXPECT_NO_THROW({
    RealtimeAudioPlayback playback(provider, {.ring_capacity_frames = 192'000,
                                              .render_block_frames = 24'000,
                                              .prefill_frames = 48'000,
                                              .prefill_timeout = std::chrono::seconds(5),
                                              .device_id = {}});
  });
}

TEST(AsyncRealtimeAudioPlayback, StopSupersedesAndCancelsAStartBlockedInPrefill) {
  auto provider = std::make_shared<BlockingPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));

  const PlaybackCommandReceipt start = playback.request_start(2'400);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(provider->wait_until_blocked());

  const PlaybackCommandReceipt stop = playback.request_stop();
  ASSERT_TRUE(stop.accepted);
  EXPECT_GT(stop.version, start.version);
  ASSERT_TRUE(playback.wait_until_completed(stop.version, std::chrono::seconds(2)));

  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Stopped);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Stopped);
  EXPECT_EQ(diagnostics.latest_requested_version, stop.version);
  EXPECT_EQ(diagnostics.latest_published_version, stop.version);
  EXPECT_EQ(diagnostics.latest_result_version, stop.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Stop);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_FALSE(diagnostics.latest_error.has_value());
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Stopped);
  EXPECT_EQ(fake->start_count, 0);
}

TEST(AsyncRealtimeAudioPlayback, DiagnosticsNeverPairSuccessWithAStaleDeviceSnapshot) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  auto device = std::make_unique<StartDiagnosticsRaceDevice>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));

  const PlaybackCommandReceipt start = playback.request_start(1'200);
  ASSERT_TRUE(start.accepted);

  const AsyncPlaybackDiagnostics raced = playback.diagnostics();
  EXPECT_EQ(raced.latest_result_version, start.version);
  EXPECT_EQ(raced.latest_status, PlaybackCommandStatus::Pending);

  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));
  const AsyncPlaybackDiagnostics completed = playback.diagnostics();
  EXPECT_EQ(completed.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(completed.effective_state, PlaybackState::Playing);
  EXPECT_TRUE(completed.playback.device_running);
}

TEST(AsyncRealtimeAudioPlayback, PauseSupersedesBlockedStartWithoutStartingStaleAudio) {
  auto provider = std::make_shared<BlockingPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));

  const PlaybackCommandReceipt start = playback.request_start(7'200);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(provider->wait_until_blocked());
  const PlaybackCommandReceipt pause = playback.request_pause();
  ASSERT_TRUE(pause.accepted);
  ASSERT_TRUE(playback.wait_until_completed(pause.version, std::chrono::seconds(2)));

  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_GT(pause.version, start.version);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.latest_published_version, pause.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Pause);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_FALSE(diagnostics.latest_error.has_value());
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Paused);
  EXPECT_FALSE(diagnostics.playback.device_running);
  EXPECT_EQ(playback.sample_counter(), 7'200);
  EXPECT_EQ(fake->start_count, 0);
}

TEST(AsyncRealtimeAudioPlayback, RapidSeekThenPauseCancelsSeekAndPublishesOnlyPause) {
  auto provider = std::make_shared<ArmableBlockingPlaybackProvider>();
  auto device = std::make_unique<FakeAudioDevice>();
  FakeAudioDevice* const fake = device.get();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), std::move(device));

  const PlaybackCommandReceipt start = playback.request_start(100);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));
  ASSERT_EQ(playback.effective_state(), PlaybackState::Playing);
  ASSERT_EQ(fake->start_count, 1);

  provider->arm();
  const PlaybackCommandReceipt seek = playback.request_seek(10'000);
  ASSERT_TRUE(seek.accepted);
  ASSERT_TRUE(provider->wait_until_blocked());
  EXPECT_NE(provider->render_thread(), std::this_thread::get_id());

  const PlaybackCommandReceipt pause = playback.request_pause();
  ASSERT_TRUE(pause.accepted);
  ASSERT_TRUE(playback.wait_until_completed(pause.version, std::chrono::seconds(2)));

  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_GT(pause.version, seek.version);
  EXPECT_EQ(diagnostics.latest_requested_version, pause.version);
  EXPECT_EQ(diagnostics.latest_published_version, pause.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Pause);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Paused);
  EXPECT_FALSE(diagnostics.playback.device_running);
  EXPECT_EQ(playback.sample_counter(), 10'000);
  EXPECT_EQ(provider->cancellations.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(fake->start_count, 1);
}

TEST(AsyncRealtimeAudioPlayback, CoalescesLatestSeekInABoundedPendingQueue) {
  auto provider = std::make_shared<ArmableBlockingPlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), nullptr,
                                      {.maximum_pending_commands = 1});

  const PlaybackCommandReceipt start = playback.request_start(0);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));
  const PlaybackCommandReceipt pause = playback.request_pause();
  ASSERT_TRUE(pause.accepted);
  ASSERT_TRUE(playback.wait_until_completed(pause.version, std::chrono::seconds(2)));
  const PlaybackCommandReceipt empty_ring = playback.request_seek(50);
  ASSERT_TRUE(empty_ring.accepted);
  ASSERT_TRUE(playback.wait_until_completed(empty_ring.version, std::chrono::seconds(2)));

  provider->arm();
  const PlaybackCommandReceipt resume = playback.request_resume();
  ASSERT_TRUE(resume.accepted);
  ASSERT_TRUE(provider->wait_until_blocked());

  const PlaybackCommandReceipt first_seek = playback.request_seek(1'000);
  const PlaybackCommandReceipt latest_seek = playback.request_seek(2'000);
  ASSERT_TRUE(first_seek.accepted);
  ASSERT_TRUE(latest_seek.accepted);
  EXPECT_GT(latest_seek.version, first_seek.version);

  provider->release();
  ASSERT_TRUE(playback.wait_until_completed(latest_seek.version, std::chrono::seconds(2)));
  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_requested_version, latest_seek.version);
  EXPECT_EQ(diagnostics.latest_published_version, latest_seek.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Seek);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Playing);
  EXPECT_EQ(playback.sample_counter(), 2'000);
}

TEST(AsyncRealtimeAudioPlayback, CoalescesPauseResumeToTheLatestTransportIntent) {
  auto provider = std::make_shared<ArmableBlockingPlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), nullptr,
                                      {.maximum_pending_commands = 1});

  const PlaybackCommandReceipt start = playback.request_start(0);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));
  const PlaybackCommandReceipt pause = playback.request_pause();
  ASSERT_TRUE(pause.accepted);
  ASSERT_TRUE(playback.wait_until_completed(pause.version, std::chrono::seconds(2)));
  const PlaybackCommandReceipt empty_ring = playback.request_seek(100);
  ASSERT_TRUE(empty_ring.accepted);
  ASSERT_TRUE(playback.wait_until_completed(empty_ring.version, std::chrono::seconds(2)));

  provider->arm();
  const PlaybackCommandReceipt blocked_resume = playback.request_resume();
  ASSERT_TRUE(blocked_resume.accepted);
  ASSERT_TRUE(provider->wait_until_blocked());
  const PlaybackCommandReceipt queued_pause = playback.request_pause();
  const PlaybackCommandReceipt latest_resume = playback.request_resume();
  ASSERT_TRUE(queued_pause.accepted);
  ASSERT_TRUE(latest_resume.accepted);
  EXPECT_GT(latest_resume.version, queued_pause.version);

  provider->release();
  ASSERT_TRUE(playback.wait_until_completed(latest_resume.version, std::chrono::seconds(2)));
  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_requested_version, latest_resume.version);
  EXPECT_EQ(diagnostics.latest_published_version, latest_resume.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Resume);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Playing);
}

TEST(AsyncRealtimeAudioPlayback, ReportsTheLatestSerializedCommandFailure) {
  auto provider = std::make_shared<FailingPlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration());

  const PlaybackCommandReceipt start = playback.request_start(0);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));

  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_requested_version, start.version);
  EXPECT_EQ(diagnostics.latest_published_version, start.version);
  EXPECT_EQ(diagnostics.latest_result_version, start.version);
  EXPECT_EQ(diagnostics.latest_command, PlaybackCommandKind::Start);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Failed);
  ASSERT_TRUE(diagnostics.latest_error.has_value());
  EXPECT_EQ(diagnostics.latest_error->code, PlaybackControlErrorCode::ProviderFailed);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Failed);
}

TEST(AsyncRealtimeAudioPlayback, PauseFromStoppedEstablishesAResumablePausedIntent) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration());

  const PlaybackCommandReceipt pause = playback.request_pause();
  ASSERT_TRUE(pause.accepted);
  ASSERT_TRUE(playback.wait_until_completed(pause.version, std::chrono::seconds(2)));
  AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Paused);
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Paused);
  EXPECT_EQ(provider->render_calls.load(std::memory_order_relaxed), 0U);

  const PlaybackCommandReceipt resume = playback.request_resume();
  ASSERT_TRUE(resume.accepted);
  ASSERT_TRUE(playback.wait_until_completed(resume.version, std::chrono::seconds(2)));
  diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Playing);
}

TEST(AsyncRealtimeAudioPlayback, SeekPlayingIntentRecoversFromFailedControllerState) {
  auto provider = std::make_shared<RecoverablePlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration());

  const PlaybackCommandReceipt start = playback.request_start(100);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));
  ASSERT_EQ(playback.diagnostics().latest_status, PlaybackCommandStatus::Failed);
  ASSERT_EQ(playback.diagnostics().playback.state, PlaybackState::Failed);

  provider->failing.store(false, std::memory_order_release);
  const PlaybackCommandReceipt seek = playback.request_seek(9'600);
  ASSERT_TRUE(seek.accepted);
  ASSERT_TRUE(playback.wait_until_completed(seek.version, std::chrono::seconds(2)));
  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_requested_version, seek.version);
  EXPECT_EQ(diagnostics.latest_published_version, seek.version);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_FALSE(diagnostics.latest_error.has_value());
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.playback.state, PlaybackState::Playing);
  EXPECT_EQ(playback.sample_counter(), 9'600);
}

TEST(AsyncRealtimeAudioPlayback, ConcurrentSeekProducersPreserveVersionAndLatestIntent) {
  auto provider = std::make_shared<DeterministicPlaybackProvider>();
  AsyncRealtimeAudioPlayback playback(provider, small_configuration(), nullptr,
                                      {.maximum_pending_commands = 1});
  const PlaybackCommandReceipt start = playback.request_start(0);
  ASSERT_TRUE(start.accepted);
  ASSERT_TRUE(playback.wait_until_completed(start.version, std::chrono::seconds(2)));

  struct VersionedSample final {
    std::uint64_t version{0};
    std::int64_t sample{0};
  };
  std::mutex receipts_mutex;
  std::vector<VersionedSample> receipts;
  std::atomic<std::uint64_t> rejected_count{0};
  constexpr std::size_t producer_count = 4;
  constexpr std::size_t requests_per_producer = 50;
  std::array<std::jthread, producer_count> producers;
  for (std::size_t producer = 0; producer < producer_count; ++producer) {
    producers[producer] = std::jthread([&, producer] {
      for (std::size_t request = 0; request < requests_per_producer; ++request) {
        const std::int64_t sample =
            static_cast<std::int64_t>(1'000 + producer * requests_per_producer + request);
        const PlaybackCommandReceipt receipt = playback.request_seek(sample);
        if (receipt.accepted) {
          std::lock_guard lock(receipts_mutex);
          receipts.push_back({.version = receipt.version, .sample = sample});
        } else {
          rejected_count.fetch_add(1U, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }

  EXPECT_EQ(rejected_count.load(std::memory_order_relaxed), 0U);
  ASSERT_EQ(receipts.size(), producer_count * requests_per_producer);
  std::ranges::sort(receipts, {}, &VersionedSample::version);
  for (std::size_t index = 1; index < receipts.size(); ++index) {
    EXPECT_GT(receipts[index].version, receipts[index - 1].version);
  }
  const VersionedSample latest = receipts.back();
  ASSERT_TRUE(playback.wait_until_completed(latest.version, std::chrono::seconds(3)));
  const AsyncPlaybackDiagnostics diagnostics = playback.diagnostics();
  EXPECT_EQ(diagnostics.latest_requested_version, latest.version);
  EXPECT_EQ(diagnostics.latest_published_version, latest.version);
  EXPECT_EQ(diagnostics.latest_result_version, latest.version);
  EXPECT_EQ(diagnostics.latest_status, PlaybackCommandStatus::Succeeded);
  EXPECT_EQ(diagnostics.requested_state, PlaybackState::Playing);
  EXPECT_EQ(diagnostics.effective_state, PlaybackState::Playing);
  EXPECT_EQ(playback.sample_counter(), latest.sample);
}

TEST(MiniaudioOutputDevice, UnavailableBuildReportsTypedFallbackWithoutOpeningHardware) {
  if (MiniaudioOutputDevice::available()) {
    GTEST_SKIP() << "Pinned miniaudio backend is compiled; hardware is not opened in unit tests";
  }
  MiniaudioOutputDevice device;
  const AudioDeviceResult result =
      device.open({.format = kPlaybackAudioFormat,
                   .callback = [](void*, float*, std::size_t) noexcept {},
                   .user_data = nullptr,
                   .device_id = {}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, AudioDeviceErrorCode::Unavailable);
  EXPECT_FALSE(device.is_open());
}

TEST(MiniaudioDeviceEnumerator, EnumerateSucceedsOrReturnsEmptyWithoutThrowing) {
  MiniaudioDeviceEnumerator enumerator;
  const auto devices = enumerator.enumerate();
  if (!MiniaudioOutputDevice::available()) {
    EXPECT_TRUE(devices.empty());
    return;
  }
  for (const auto& device : devices) {
    EXPECT_FALSE(device.id.empty());
    EXPECT_TRUE(device.id.starts_with("miniaudio:"));
  }
}

TEST(Dsp, AppliesGainPanAndLimiterDeterministically) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 1);
  block.channel(0)[0] = 2.0F;
  block.channel(1)[0] = 2.0F;
  apply_gain(block, 0.5F);
  apply_stereo_pan(block, -1.0F);
  LookaheadFreeLimiter limiter(-6.0206F);
  limiter.process(block);
  EXPECT_NEAR(block.channel(0)[0], 0.5F, 0.001F);
  EXPECT_NEAR(block.channel(1)[0], 0.0F, 0.001F);
}

TEST(LevelMeter, MeasuresPeakAndRmsPerChannel) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 4);
  block.channel(0)[0] = 1.0F;
  block.channel(1)[0] = -0.5F;
  const LevelReading levels = measure_levels(block);
  EXPECT_FLOAT_EQ(levels.peak[0], 1.0F);
  EXPECT_FLOAT_EQ(levels.peak[1], 0.5F);
  EXPECT_NEAR(levels.rms[0], 0.5F, 0.0001F);
  EXPECT_NEAR(levels.rms[1], 0.25F, 0.0001F);
}

TEST(LoudnessMeter, AcceptsMatchingBlocksAndReportsSamplePeak) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 48'000);
  for (std::size_t frame = 0; frame < block.frame_count(); ++frame) {
    const float sample = 0.25F * std::sin(static_cast<float>(frame) * 0.03F);
    block.channel(0)[frame] = sample;
    block.channel(1)[frame] = sample;
  }
  LoudnessMeter meter(block.format());
  ASSERT_TRUE(meter.valid());
  ASSERT_TRUE(meter.add(block));
  const LoudnessReading reading = meter.reading();
  ASSERT_EQ(reading.sample_peak_dbfs.size(), 2U);
  EXPECT_NEAR(reading.sample_peak_dbfs[0], -12.04, 0.1);
}

} // namespace
} // namespace video_editor::audio

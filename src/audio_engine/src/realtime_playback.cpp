// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/realtime_playback.h"

#include "video_editor/audio_engine/spsc_audio_ring.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace video_editor::audio {
namespace {

static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<PlaybackState>::is_always_lock_free);

enum class CallbackOrigin : std::uint8_t { Device, Manual };

[[nodiscard]] PlaybackControlResult control_failure(const PlaybackControlErrorCode code,
                                                    std::string message) {
  return PlaybackControlResult::failure(
      {.code = code, .message = std::move(message), .device_error = std::nullopt});
}

[[nodiscard]] bool valid_configuration(const RealtimePlaybackConfiguration& configuration) {
  return configuration.ring_capacity_frames > 0U && configuration.render_block_frames > 0U &&
         configuration.render_block_frames <= configuration.ring_capacity_frames &&
         configuration.prefill_frames <= configuration.ring_capacity_frames &&
         configuration.prefill_timeout.count() > 0;
}

} // namespace

class RealtimeAudioPlayback::Impl final {
public:
  Impl(std::shared_ptr<PlaybackAudioProvider> provider_value,
       const RealtimePlaybackConfiguration& configuration_value,
       std::unique_ptr<AudioOutputDevice> device_value)
      : provider(std::move(provider_value)), configuration(configuration_value),
        device(std::move(device_value)),
        ring(configuration.ring_capacity_frames, kPlaybackAudioFormat.channels) {
    if (provider == nullptr) {
      throw std::invalid_argument("realtime playback requires an audio provider");
    }
    if (!valid_configuration(configuration)) {
      throw std::invalid_argument("realtime playback configuration is invalid");
    }
  }

  ~Impl() {
    stop();
  }

  static void device_callback(void* user_data, float* output,
                              const std::size_t frame_count) noexcept {
    auto* self = static_cast<Impl*>(user_data);
    if (self == nullptr || output == nullptr) {
      return;
    }
    const std::size_t sample_count = frame_count * kPlaybackAudioFormat.channels;
    static_cast<void>(
        self->render_callback(std::span<float>(output, sample_count), CallbackOrigin::Device));
  }

  void set_last_error(std::string message) {
    {
      std::lock_guard lock(status_mutex);
      last_error = std::move(message);
    }
    status_changed.notify_all();
  }

  void notify_status() {
    // Pair worker-side condition changes with the mutex used by
    // wait_for_prefill(). The realtime callback never calls this helper.
    {
      std::lock_guard lock(status_mutex);
    }
    status_changed.notify_all();
  }

  [[nodiscard]] std::string last_error_copy() const {
    std::lock_guard lock(status_mutex);
    return last_error;
  }

  void fail_worker(std::string message) {
    {
      std::lock_guard lock(status_mutex);
      last_error = std::move(message);
      worker_failed.store(true, std::memory_order_release);
      PlaybackState expected = PlaybackState::Playing;
      if (!state.compare_exchange_strong(expected, PlaybackState::Failed,
                                         std::memory_order_acq_rel)) {
        expected = PlaybackState::Starting;
        state.compare_exchange_strong(expected, PlaybackState::Failed, std::memory_order_acq_rel);
      }
    }
    status_changed.notify_all();
  }

  void worker_main(const std::stop_token cancellation) noexcept {
    while (!cancellation.stop_requested()) {
      if (provider_exhausted.load(std::memory_order_acquire)) {
        return;
      }
      const std::size_t writable = ring.available_write_frames();
      if (writable == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      const std::size_t request_frames = std::min(writable, configuration.render_block_frames);
      const std::int64_t request_start = next_render_sample.load(std::memory_order_acquire);
      PlaybackRenderResult rendered;
      try {
        rendered = provider->render({.start_sample = request_start,
                                     .sample_count = request_frames,
                                     .epoch = epoch.load(std::memory_order_acquire),
                                     .cancellation = cancellation});
      } catch (const std::exception& exception) {
        fail_worker(std::string{"audio provider threw an exception: "} + exception.what());
        return;
      } catch (...) {
        fail_worker("audio provider threw an unknown exception");
        return;
      }
      if (cancellation.stop_requested()) {
        return;
      }
      switch (rendered.status) {
      case PlaybackRenderStatus::EndOfStream:
        provider_exhausted.store(true, std::memory_order_release);
        notify_status();
        return;
      case PlaybackRenderStatus::Cancelled:
        fail_worker(rendered.message.empty() ? "audio provider cancelled unexpectedly"
                                             : std::move(rendered.message));
        return;
      case PlaybackRenderStatus::Failed:
        fail_worker(rendered.message.empty() ? "audio provider failed"
                                             : std::move(rendered.message));
        return;
      case PlaybackRenderStatus::Ready:
        break;
      }
      if (!rendered.block.has_value()) {
        fail_worker("audio provider returned Ready without a block");
        return;
      }
      const AudioBlock& block = *rendered.block;
      if (block.format().sample_rate != kPlaybackAudioFormat.sample_rate ||
          block.format().channels != kPlaybackAudioFormat.channels ||
          block.start_sample() != request_start || block.frame_count() != request_frames) {
        fail_worker("audio provider returned a block with an unexpected format or range");
        return;
      }
      std::vector<float> interleaved = block.interleaved();
      const std::size_t written = ring.write(interleaved);
      if (written != request_frames) {
        fail_worker("audio pre-render ring rejected an exact provider block");
        return;
      }
      next_render_sample.store(request_start + static_cast<std::int64_t>(written),
                               std::memory_order_release);
      pre_rendered_frames.fetch_add(written, std::memory_order_relaxed);
      notify_status();
    }
  }

  void start_worker() {
    if (worker.joinable() || provider_exhausted.load(std::memory_order_acquire)) {
      return;
    }
    auto cancellation = std::make_shared<std::stop_source>();
    worker_cancellation.store(cancellation, std::memory_order_release);
    if (external_cancellation_requested.load(std::memory_order_acquire)) {
      cancellation->request_stop();
    }
    worker = std::jthread([this, cancellation] { worker_main(cancellation->get_token()); });
  }

  void stop_worker() noexcept {
    const auto cancellation = worker_cancellation.load(std::memory_order_acquire);
    if (cancellation != nullptr) {
      cancellation->request_stop();
    }
    if (!worker.joinable()) {
      worker_cancellation.store(nullptr, std::memory_order_release);
      return;
    }
    worker.request_stop();
    worker.join();
    worker_cancellation.store(nullptr, std::memory_order_release);
  }

  void disable_callbacks_and_wait() noexcept {
    callbacks_allowed.store(false, std::memory_order_release);
    // Control-thread wait only. An admitted callback never waits on control,
    // and a callback that observes the closed gate cannot touch the ring.
    while (active_callbacks.load(std::memory_order_acquire) != 0U) {
      std::this_thread::yield();
    }
  }

  void enable_callbacks() noexcept {
    callbacks_allowed.store(true, std::memory_order_release);
  }

  [[nodiscard]] std::optional<PlaybackControlError> wait_for_prefill() {
    if (external_cancellation_requested.load(std::memory_order_acquire)) {
      return PlaybackControlError{.code = PlaybackControlErrorCode::Cancelled,
                                  .message = "audio control operation was superseded",
                                  .device_error = std::nullopt};
    }
    if (configuration.prefill_frames == 0U ||
        ring.available_read_frames() >= configuration.prefill_frames ||
        provider_exhausted.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    std::unique_lock lock(status_mutex);
    const bool ready = status_changed.wait_for(lock, configuration.prefill_timeout, [this] {
      return ring.available_read_frames() >= configuration.prefill_frames ||
             provider_exhausted.load(std::memory_order_acquire) ||
             worker_failed.load(std::memory_order_acquire) ||
             external_cancellation_requested.load(std::memory_order_acquire);
    });
    if (!ready) {
      return PlaybackControlError{.code = PlaybackControlErrorCode::PrefillTimeout,
                                  .message = "audio pre-render did not reach its target in time",
                                  .device_error = std::nullopt};
    }
    if (external_cancellation_requested.load(std::memory_order_acquire)) {
      return PlaybackControlError{.code = PlaybackControlErrorCode::Cancelled,
                                  .message = "audio control operation was superseded",
                                  .device_error = std::nullopt};
    }
    if (worker_failed.load(std::memory_order_acquire)) {
      return PlaybackControlError{.code = PlaybackControlErrorCode::ProviderFailed,
                                  .message = last_error,
                                  .device_error = std::nullopt};
    }
    return std::nullopt;
  }

  void reset_epoch(const std::int64_t sample) {
    ring.reset();
    playback_origin_sample.store(sample, std::memory_order_release);
    submitted_sample_counter.store(sample, std::memory_order_release);
    last_submitted_buffer_frames.store(0U, std::memory_order_release);
    next_render_sample.store(sample, std::memory_order_release);
    provider_exhausted.store(false, std::memory_order_release);
    worker_failed.store(false, std::memory_order_release);
    {
      std::lock_guard lock(status_mutex);
      last_error.clear();
    }
    epoch.fetch_add(1U, std::memory_order_acq_rel);
  }

  [[nodiscard]] PlaybackControlResult open_device() {
    if (device == nullptr) {
      return PlaybackControlResult::success();
    }
    const AudioDeviceResult opened = device->open(
        {.format = kPlaybackAudioFormat, .callback = &Impl::device_callback, .user_data = this});
    if (!opened) {
      return PlaybackControlResult::failure({.code = PlaybackControlErrorCode::DeviceOpenFailed,
                                             .message = opened.error->message,
                                             .device_error = opened.error});
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult start_device() {
    if (device == nullptr) {
      return PlaybackControlResult::success();
    }
    const AudioDeviceResult started = device->start();
    if (!started) {
      return PlaybackControlResult::failure({.code = PlaybackControlErrorCode::DeviceStartFailed,
                                             .message = started.error->message,
                                             .device_error = started.error});
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult start(const std::int64_t start_sample) {
    std::lock_guard control_lock(control_mutex);
    if (start_sample < 0) {
      return control_failure(PlaybackControlErrorCode::InvalidSample,
                             "playback start sample cannot be negative");
    }
    if (state.load(std::memory_order_acquire) != PlaybackState::Stopped) {
      return control_failure(PlaybackControlErrorCode::InvalidState,
                             "playback can start only from the stopped state");
    }
    if (auto opened = open_device(); !opened) {
      return opened;
    }
    reset_epoch(start_sample);
    state.store(PlaybackState::Starting, std::memory_order_release);
    start_worker();
    if (auto prefill_error = wait_for_prefill()) {
      stop_worker();
      if (prefill_error->code == PlaybackControlErrorCode::Cancelled) {
        // A superseding async intent interrupted prefill before callbacks or
        // the device started. Preserve the exact position and the open device
        // as a paused transport so the serialized successor can pause, seek,
        // or resume without exposing a transient Playing state.
        state.store(PlaybackState::Paused, std::memory_order_release);
      } else {
        if (device != nullptr) {
          device->close();
        }
        state.store(PlaybackState::Failed, std::memory_order_release);
        set_last_error(prefill_error->message);
      }
      return PlaybackControlResult::failure(std::move(*prefill_error));
    }
    state.store(PlaybackState::Playing, std::memory_order_release);
    enable_callbacks();
    if (auto started = start_device(); !started) {
      state.store(PlaybackState::Failed, std::memory_order_release);
      disable_callbacks_and_wait();
      stop_worker();
      device->close();
      set_last_error(started.error->message);
      return started;
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult pause() {
    std::lock_guard control_lock(control_mutex);
    if (state.load(std::memory_order_acquire) != PlaybackState::Playing) {
      return control_failure(PlaybackControlErrorCode::InvalidState,
                             "playback can pause only while playing");
    }
    state.store(PlaybackState::Paused, std::memory_order_release);
    callbacks_allowed.store(false, std::memory_order_release);
    if (device != nullptr) {
      device->stop();
    }
    disable_callbacks_and_wait();
    stop_worker();
    if (worker_failed.load(std::memory_order_acquire)) {
      state.store(PlaybackState::Failed, std::memory_order_release);
      return control_failure(PlaybackControlErrorCode::ProviderFailed, last_error_copy());
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult resume() {
    std::lock_guard control_lock(control_mutex);
    if (state.load(std::memory_order_acquire) != PlaybackState::Paused) {
      return control_failure(PlaybackControlErrorCode::InvalidState,
                             "playback can resume only from the paused state");
    }
    start_worker();
    if (auto prefill_error = wait_for_prefill()) {
      stop_worker();
      if (prefill_error->code != PlaybackControlErrorCode::Cancelled) {
        set_last_error(prefill_error->message);
      }
      return PlaybackControlResult::failure(std::move(*prefill_error));
    }
    state.store(PlaybackState::Playing, std::memory_order_release);
    enable_callbacks();
    if (auto started = start_device(); !started) {
      state.store(PlaybackState::Failed, std::memory_order_release);
      disable_callbacks_and_wait();
      stop_worker();
      set_last_error(started.error->message);
      return started;
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult seek(const std::int64_t sample) {
    std::lock_guard control_lock(control_mutex);
    if (sample < 0) {
      return control_failure(PlaybackControlErrorCode::InvalidSample,
                             "playback seek sample cannot be negative");
    }
    const PlaybackState current = state.load(std::memory_order_acquire);
    if (current != PlaybackState::Playing && current != PlaybackState::Paused) {
      return control_failure(PlaybackControlErrorCode::InvalidState,
                             "playback can seek only while playing or paused");
    }
    const bool restart = current == PlaybackState::Playing;
    state.store(PlaybackState::Paused, std::memory_order_release);
    callbacks_allowed.store(false, std::memory_order_release);
    if (device != nullptr && restart) {
      device->stop();
    }
    disable_callbacks_and_wait();
    stop_worker();
    reset_epoch(sample);
    if (!restart) {
      return PlaybackControlResult::success();
    }
    state.store(PlaybackState::Starting, std::memory_order_release);
    start_worker();
    if (auto prefill_error = wait_for_prefill()) {
      stop_worker();
      if (prefill_error->code == PlaybackControlErrorCode::Cancelled) {
        // The device has already been stopped and callbacks quiesced. Staying
        // paused gives the next versioned intent a safe, exact seek position.
        state.store(PlaybackState::Paused, std::memory_order_release);
      } else {
        state.store(PlaybackState::Failed, std::memory_order_release);
        set_last_error(prefill_error->message);
      }
      return PlaybackControlResult::failure(std::move(*prefill_error));
    }
    state.store(PlaybackState::Playing, std::memory_order_release);
    enable_callbacks();
    if (auto started = start_device(); !started) {
      state.store(PlaybackState::Failed, std::memory_order_release);
      disable_callbacks_and_wait();
      stop_worker();
      set_last_error(started.error->message);
      return started;
    }
    return PlaybackControlResult::success();
  }

  [[nodiscard]] PlaybackControlResult prepare_paused(const std::int64_t sample) {
    std::lock_guard control_lock(control_mutex);
    if (sample < 0) {
      return control_failure(PlaybackControlErrorCode::InvalidSample,
                             "paused playback sample cannot be negative");
    }

    const PlaybackState previous = state.load(std::memory_order_acquire);
    state.store(PlaybackState::Paused, std::memory_order_release);
    callbacks_allowed.store(false, std::memory_order_release);
    if (device != nullptr) {
      device->stop();
    }
    disable_callbacks_and_wait();
    stop_worker();

    // A failed device may be open but unusable. Recreate it before promising a
    // resumable paused state; normal Playing/Paused transitions can retain the
    // already-open adapter.
    if (device != nullptr && previous == PlaybackState::Failed) {
      device->close();
    }
    if (device != nullptr && !device->is_open()) {
      if (auto opened = open_device(); !opened) {
        state.store(PlaybackState::Failed, std::memory_order_release);
        set_last_error(opened.error->message);
        return opened;
      }
    }

    reset_epoch(sample);
    state.store(PlaybackState::Paused, std::memory_order_release);
    return PlaybackControlResult::success();
  }

  void request_control_cancellation() noexcept {
    external_cancellation_requested.store(true, std::memory_order_release);
    const auto cancellation = worker_cancellation.load(std::memory_order_acquire);
    if (cancellation != nullptr) {
      cancellation->request_stop();
    }
    status_changed.notify_all();
  }

  void clear_control_cancellation() noexcept {
    external_cancellation_requested.store(false, std::memory_order_release);
  }

  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept {
    const std::uint64_t current_buffer =
        last_submitted_buffer_frames.load(std::memory_order_acquire);
    const std::uint64_t backend =
        device != nullptr ? device->estimated_output_latency_frames() : 0U;
    return std::max(current_buffer, backend);
  }

  [[nodiscard]] std::int64_t playback_position_sample() const noexcept {
    const std::int64_t origin = playback_origin_sample.load(std::memory_order_acquire);
    const std::int64_t submitted = submitted_sample_counter.load(std::memory_order_acquire);
    const std::uint64_t submitted_delta =
        submitted > origin ? static_cast<std::uint64_t>(submitted - origin) : 0U;
    const std::uint64_t latency = std::min(estimated_output_latency_frames(), submitted_delta);
    return submitted - static_cast<std::int64_t>(latency);
  }

  void stop() noexcept {
    std::lock_guard control_lock(control_mutex);
    state.store(PlaybackState::Stopped, std::memory_order_release);
    callbacks_allowed.store(false, std::memory_order_release);
    if (device != nullptr) {
      device->stop();
    }
    disable_callbacks_and_wait();
    stop_worker();
    if (device != nullptr) {
      device->close();
    }
    ring.reset();
    playback_origin_sample.store(0, std::memory_order_release);
    submitted_sample_counter.store(0, std::memory_order_release);
    last_submitted_buffer_frames.store(0U, std::memory_order_release);
    next_render_sample.store(0, std::memory_order_release);
    provider_exhausted.store(false, std::memory_order_release);
    worker_failed.store(false, std::memory_order_release);
    epoch.fetch_add(1U, std::memory_order_acq_rel);
  }

  [[nodiscard]] std::size_t render_callback(std::span<float> output,
                                            const CallbackOrigin origin) noexcept {
    std::fill(output.begin(), output.end(), 0.0F);
    callback_count.fetch_add(1U, std::memory_order_relaxed);
    const std::size_t frame_count = output.size() / kPlaybackAudioFormat.channels;
    if (frame_count == 0U || (origin == CallbackOrigin::Manual && device != nullptr) ||
        !callbacks_allowed.load(std::memory_order_acquire) ||
        state.load(std::memory_order_acquire) != PlaybackState::Playing) {
      return 0U;
    }

    active_callbacks.fetch_add(1U, std::memory_order_acq_rel);
    if (!callbacks_allowed.load(std::memory_order_acquire) ||
        state.load(std::memory_order_acquire) != PlaybackState::Playing) {
      active_callbacks.fetch_sub(1U, std::memory_order_release);
      return 0U;
    }
    if (consumer_active.test_and_set(std::memory_order_acquire)) {
      if (origin == CallbackOrigin::Device) {
        last_submitted_buffer_frames.store(frame_count, std::memory_order_release);
        submitted_sample_counter.fetch_add(static_cast<std::int64_t>(frame_count),
                                           std::memory_order_acq_rel);
      }
      xrun_count.fetch_add(1U, std::memory_order_relaxed);
      underrun_frames.fetch_add(frame_count, std::memory_order_relaxed);
      active_callbacks.fetch_sub(1U, std::memory_order_release);
      return 0U;
    }

    const std::size_t complete_sample_count = frame_count * kPlaybackAudioFormat.channels;
    const std::size_t read = ring.read(output.first(complete_sample_count));
    last_submitted_buffer_frames.store(frame_count, std::memory_order_release);
    submitted_sample_counter.fetch_add(static_cast<std::int64_t>(frame_count),
                                       std::memory_order_acq_rel);
    if (read < frame_count) {
      xrun_count.fetch_add(1U, std::memory_order_relaxed);
      underrun_frames.fetch_add(frame_count - read, std::memory_order_relaxed);
    }
    consumer_active.clear(std::memory_order_release);
    active_callbacks.fetch_sub(1U, std::memory_order_release);
    return read;
  }

  [[nodiscard]] PlaybackDiagnostics diagnostics() const {
    const std::int64_t playback_position = playback_position_sample();
    const std::uint64_t latency = estimated_output_latency_frames();
    PlaybackDiagnostics result{
        .state = state.load(std::memory_order_acquire),
        .playback_position_sample = playback_position,
        .sample_counter = playback_position,
        .submitted_sample_counter = submitted_sample_counter.load(std::memory_order_acquire),
        .estimated_output_latency_frames = latency,
        .clock_uncertainty_frames = latency,
        .clock_is_estimated = true,
        .epoch = epoch.load(std::memory_order_acquire),
        .callback_count = callback_count.load(std::memory_order_relaxed),
        .pre_rendered_frames = pre_rendered_frames.load(std::memory_order_relaxed),
        .xrun_count = xrun_count.load(std::memory_order_relaxed),
        .underrun_frames = underrun_frames.load(std::memory_order_relaxed),
        .queued_frames = ring.available_read_frames(),
        .provider_exhausted = provider_exhausted.load(std::memory_order_acquire),
        .device_present = device != nullptr,
        .device_open = device != nullptr && device->is_open(),
        .device_running = device != nullptr && device->is_running(),
        .last_error = {},
    };
    result.last_error = last_error_copy();
    return result;
  }

  std::shared_ptr<PlaybackAudioProvider> provider;
  RealtimePlaybackConfiguration configuration;
  std::unique_ptr<AudioOutputDevice> device;
  SpscAudioRing ring;
  mutable std::mutex control_mutex;
  mutable std::mutex status_mutex;
  std::condition_variable status_changed;
  std::jthread worker;
  std::atomic<PlaybackState> state{PlaybackState::Stopped};
  std::atomic<std::int64_t> playback_origin_sample{0};
  std::atomic<std::int64_t> submitted_sample_counter{0};
  std::atomic<std::uint64_t> last_submitted_buffer_frames{0};
  std::atomic<std::int64_t> next_render_sample{0};
  std::atomic<std::uint64_t> epoch{0};
  std::atomic<std::uint64_t> callback_count{0};
  std::atomic<std::uint64_t> pre_rendered_frames{0};
  std::atomic<std::uint64_t> xrun_count{0};
  std::atomic<std::uint64_t> underrun_frames{0};
  std::atomic<std::uint64_t> active_callbacks{0};
  std::atomic<bool> callbacks_allowed{false};
  std::atomic_flag consumer_active = ATOMIC_FLAG_INIT;
  std::atomic<bool> provider_exhausted{false};
  std::atomic<bool> worker_failed{false};
  std::atomic<bool> external_cancellation_requested{false};
  std::atomic<std::shared_ptr<std::stop_source>> worker_cancellation{nullptr};
  std::string last_error;
};

RealtimeAudioPlayback::RealtimeAudioPlayback(std::shared_ptr<PlaybackAudioProvider> provider,
                                             const RealtimePlaybackConfiguration configuration,
                                             std::unique_ptr<AudioOutputDevice> device)
    : impl_(std::make_unique<Impl>(std::move(provider), configuration, std::move(device))) {}

RealtimeAudioPlayback::~RealtimeAudioPlayback() = default;

PlaybackControlResult RealtimeAudioPlayback::start(const std::int64_t start_sample) {
  return impl_->start(start_sample);
}

PlaybackControlResult RealtimeAudioPlayback::pause() {
  return impl_->pause();
}

PlaybackControlResult RealtimeAudioPlayback::resume() {
  return impl_->resume();
}

PlaybackControlResult RealtimeAudioPlayback::seek(const std::int64_t sample) {
  return impl_->seek(sample);
}

PlaybackControlResult RealtimeAudioPlayback::prepare_paused(const std::int64_t sample) {
  return impl_->prepare_paused(sample);
}

void RealtimeAudioPlayback::stop() noexcept {
  impl_->stop();
}

std::size_t
RealtimeAudioPlayback::render_callback(const std::span<float> interleaved_output) noexcept {
  return impl_->render_callback(interleaved_output, CallbackOrigin::Manual);
}

PlaybackState RealtimeAudioPlayback::state() const noexcept {
  return impl_->state.load(std::memory_order_acquire);
}

std::int64_t RealtimeAudioPlayback::sample_counter() const noexcept {
  return impl_->playback_position_sample();
}

std::int64_t RealtimeAudioPlayback::submitted_sample_counter() const noexcept {
  return impl_->submitted_sample_counter.load(std::memory_order_acquire);
}

std::uint64_t RealtimeAudioPlayback::estimated_output_latency_frames() const noexcept {
  return impl_->estimated_output_latency_frames();
}

double RealtimeAudioPlayback::clock_seconds() const noexcept {
  return static_cast<double>(sample_counter()) /
         static_cast<double>(kPlaybackAudioFormat.sample_rate);
}

std::uint64_t RealtimeAudioPlayback::epoch() const noexcept {
  return impl_->epoch.load(std::memory_order_acquire);
}

bool RealtimeAudioPlayback::device_present() const noexcept {
  return impl_->device != nullptr;
}

bool RealtimeAudioPlayback::device_open() const noexcept {
  return impl_->device != nullptr && impl_->device->is_open();
}

PlaybackDiagnostics RealtimeAudioPlayback::diagnostics() const {
  return impl_->diagnostics();
}

void RealtimeAudioPlayback::request_control_cancellation() noexcept {
  impl_->request_control_cancellation();
}

void RealtimeAudioPlayback::clear_control_cancellation() noexcept {
  impl_->clear_control_cancellation();
}

} // namespace video_editor::audio

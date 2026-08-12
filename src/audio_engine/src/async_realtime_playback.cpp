// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/async_realtime_playback.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace video_editor::audio {
namespace {

static_assert(std::atomic<PlaybackState>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct QueuedCommand final {
  PlaybackCommandKind kind{PlaybackCommandKind::Stop};
  std::uint64_t version{0};
  std::int64_t sample{0};
  PlaybackState target_state{PlaybackState::Stopped};
  // True when this intent carries an explicit Start/Seek position that must
  // survive coalescing into a later Pause or Resume.
  bool reposition{false};
};

[[nodiscard]] bool is_cancellable_control(const PlaybackCommandKind kind) noexcept {
  return kind == PlaybackCommandKind::Start || kind == PlaybackCommandKind::Seek ||
         kind == PlaybackCommandKind::Resume;
}

[[nodiscard]] PlaybackState requested_state_for(const PlaybackCommandKind kind,
                                                const PlaybackState current) noexcept {
  switch (kind) {
  case PlaybackCommandKind::Start:
  case PlaybackCommandKind::Resume:
    return PlaybackState::Playing;
  case PlaybackCommandKind::Pause:
    return PlaybackState::Paused;
  case PlaybackCommandKind::Seek:
    return current;
  case PlaybackCommandKind::Stop:
    return PlaybackState::Stopped;
  }
  return current;
}

[[nodiscard]] AsyncRealtimePlaybackConfiguration
validated_async_configuration(const AsyncRealtimePlaybackConfiguration& configuration) {
  if (configuration.maximum_pending_commands == 0U) {
    throw std::invalid_argument("async playback command capacity must be non-zero");
  }
  return configuration;
}

} // namespace

class AsyncRealtimeAudioPlayback::Impl final {
public:
  Impl(std::shared_ptr<PlaybackAudioProvider> provider,
       const RealtimePlaybackConfiguration& playback_configuration,
       std::unique_ptr<AudioOutputDevice> device,
       const AsyncRealtimePlaybackConfiguration& async_configuration_value)
      : playback(std::move(provider), playback_configuration, std::move(device)),
        async_configuration(validated_async_configuration(async_configuration_value)),
        control_thread([this](const std::stop_token cancellation) { control_loop(cancellation); }) {
  }

  ~Impl() {
    {
      std::lock_guard lock(queue_mutex);
      shutting_down.store(true, std::memory_order_release);
      commands.clear();
      // Serialize cancellation against the dequeue-side clear. If a command
      // was already admitted, this request is necessarily ordered after that
      // clear; if it was not, the control thread observes shutting_down and
      // will not clear the request by dequeuing more work.
      playback.request_control_cancellation();
    }
    control_thread.request_stop();
    queue_changed.notify_all();
    completed_changed.notify_all();
    if (control_thread.joinable()) {
      control_thread.join();
    }
  }

  // Called only while queue_mutex is held, making the command version the
  // linearization order even with concurrent GUI/input producers.
  [[nodiscard]] std::uint64_t allocate_version() noexcept {
    const std::uint64_t version = next_version.fetch_add(1U, std::memory_order_relaxed);
    return version == 0U ? next_version.fetch_add(1U, std::memory_order_relaxed) : version;
  }

  [[nodiscard]] PlaybackCommandReceipt reject(const std::uint64_t version,
                                              const PlaybackCommandEnqueueErrorCode code,
                                              std::string message) const noexcept {
    try {
      return {.version = version,
              .accepted = false,
              .error = PlaybackCommandEnqueueError{.code = code, .message = std::move(message)}};
    } catch (...) {
      return {.version = version, .accepted = false, .error = std::nullopt};
    }
  }

  [[nodiscard]] PlaybackCommandReceipt enqueue(const PlaybackCommandKind kind,
                                               const std::int64_t sample) noexcept {
    std::uint64_t version = 0;
    try {
      {
        std::lock_guard queue_lock(queue_mutex);
        version = allocate_version();
        if ((kind == PlaybackCommandKind::Start || kind == PlaybackCommandKind::Seek) &&
            sample < 0) {
          return reject(version, PlaybackCommandEnqueueErrorCode::InvalidSample,
                        "audio command sample cannot be negative");
        }
        if (shutting_down.load(std::memory_order_acquire)) {
          return reject(version, PlaybackCommandEnqueueErrorCode::ShuttingDown,
                        "async audio playback is shutting down");
        }
        const PlaybackState previous = requested_state.load(std::memory_order_acquire);
        const PlaybackState target = requested_state_for(kind, previous);
        std::int64_t intended_sample = sample;
        bool reposition = kind == PlaybackCommandKind::Start || kind == PlaybackCommandKind::Seek;
        if (!reposition && kind != PlaybackCommandKind::Stop) {
          intended_sample = playback.sample_counter();
          if (!commands.empty() && commands.back().reposition) {
            intended_sample = commands.back().sample;
            reposition = true;
          } else if (active_command.has_value() && active_command->reposition &&
                     is_cancellable_control(active_command->kind)) {
            intended_sample = active_command->sample;
            reposition = true;
          }
        }
        const QueuedCommand incoming{.kind = kind,
                                     .version = version,
                                     .sample = intended_sample,
                                     .target_state = target,
                                     .reposition = reposition};

        // These five public commands describe transport intent, not an
        // imperative script. The newest pending intent therefore fully
        // supersedes older pending work. Its target state plus optional exact
        // position retain everything needed to reconcile the direct
        // controller. This also bounds the current queue at one command even
        // when the configured future-facing capacity is larger.
        if (!commands.empty()) {
          commands.front() = incoming;
          commands.resize(1U);
        } else {
          commands.push_back(incoming);
        }

        requested_state.store(target, std::memory_order_release);
        latest_requested_version.store(version, std::memory_order_release);
        {
          std::lock_guard result_lock(result_mutex);
          latest_result_version = version;
          latest_command = kind;
          latest_status = PlaybackCommandStatus::Pending;
          latest_error.reset();
        }

        // Keep this request under queue_mutex. The dequeue path clears the
        // flag under the same mutex immediately before admitting a command,
        // so a newer intent can never be accidentally cleared in the gap
        // between dequeue and execute().
        if (active_command.has_value() && is_cancellable_control(active_command->kind)) {
          playback.request_control_cancellation();
        }
      }
      queue_changed.notify_one();
      return {.version = version, .accepted = true, .error = std::nullopt};
    } catch (...) {
      return reject(version, PlaybackCommandEnqueueErrorCode::QueueFull,
                    "could not enqueue async audio command");
    }
  }

  [[nodiscard]] PlaybackControlResult execute(const QueuedCommand& command) {
    switch (command.kind) {
    case PlaybackCommandKind::Start: {
      const PlaybackState current = playback.state();
      if (current == PlaybackState::Playing) {
        return playback.seek(command.sample);
      }
      if (current == PlaybackState::Stopped) {
        return playback.start(command.sample);
      }
      PlaybackControlResult positioned = playback.prepare_paused(command.sample);
      return positioned ? playback.resume() : positioned;
    }
    case PlaybackCommandKind::Pause: {
      if (command.reposition) {
        return playback.prepare_paused(command.sample);
      }
      const PlaybackState current = playback.state();
      if (current == PlaybackState::Paused) {
        return PlaybackControlResult::success();
      }
      if (current == PlaybackState::Playing) {
        return playback.pause();
      }
      return playback.prepare_paused(command.sample);
    }
    case PlaybackCommandKind::Resume: {
      const PlaybackState current = playback.state();
      if (current == PlaybackState::Playing && !command.reposition) {
        return PlaybackControlResult::success();
      }
      if (command.reposition || current == PlaybackState::Stopped ||
          current == PlaybackState::Failed) {
        PlaybackControlResult positioned = playback.prepare_paused(command.sample);
        return positioned ? playback.resume() : positioned;
      }
      return playback.resume();
    }
    case PlaybackCommandKind::Seek: {
      if (command.target_state == PlaybackState::Paused) {
        return playback.prepare_paused(command.sample);
      }
      if (command.target_state != PlaybackState::Playing) {
        return PlaybackControlResult::failure(
            {.code = PlaybackControlErrorCode::InvalidState,
             .message = "audio seek requires a playing or paused transport intent",
             .device_error = std::nullopt});
      }
      if (playback.state() == PlaybackState::Playing) {
        return playback.seek(command.sample);
      }
      PlaybackControlResult positioned = playback.prepare_paused(command.sample);
      return positioned ? playback.resume() : positioned;
    }
    case PlaybackCommandKind::Stop:
      playback.stop();
      return PlaybackControlResult::success();
    }
    return PlaybackControlResult::failure({.code = PlaybackControlErrorCode::InvalidState,
                                           .message = "unknown async audio command",
                                           .device_error = std::nullopt});
  }

  void publish_completion(const QueuedCommand& command, PlaybackControlResult result) {
    {
      std::lock_guard queue_lock(queue_mutex);
      if (active_command.has_value() && active_command->version == command.version) {
        active_command.reset();
      }
      const bool is_latest =
          latest_requested_version.load(std::memory_order_acquire) == command.version;
      {
        std::lock_guard result_lock(result_mutex);
        latest_completed_version.store(
            std::max(latest_completed_version.load(std::memory_order_relaxed), command.version),
            std::memory_order_release);
        if (is_latest && latest_result_version == command.version) {
          effective_state.store(result ? command.target_state : playback.state(),
                                std::memory_order_release);
          latest_published_version.store(command.version, std::memory_order_release);
          latest_status = result ? PlaybackCommandStatus::Succeeded : PlaybackCommandStatus::Failed;
          latest_error = std::move(result.error);
        }
      }
    }
    completed_changed.notify_all();
  }

  void control_loop(const std::stop_token cancellation) noexcept {
    while (!cancellation.stop_requested()) {
      QueuedCommand command;
      {
        std::unique_lock lock(queue_mutex);
        queue_changed.wait(lock, cancellation, [this] {
          return !commands.empty() || shutting_down.load(std::memory_order_acquire);
        });
        if (cancellation.stop_requested() || shutting_down.load(std::memory_order_acquire)) {
          break;
        }
        // This clear and active-command publication are serialized with every
        // enqueue-side cancellation request. Do not move the clear into
        // execute(): doing so creates a lost-cancellation window.
        playback.clear_control_cancellation();
        command = commands.front();
        commands.pop_front();
        active_command = command;
      }
      PlaybackControlResult result;
      try {
        result = execute(command);
      } catch (const std::exception& exception) {
        result = PlaybackControlResult::failure(
            {.code = PlaybackControlErrorCode::ProviderFailed,
             .message = std::string{"async audio control threw: "} + exception.what(),
             .device_error = std::nullopt});
      } catch (...) {
        result = PlaybackControlResult::failure(
            {.code = PlaybackControlErrorCode::ProviderFailed,
             .message = "async audio control threw an unknown exception",
             .device_error = std::nullopt});
      }
      publish_completion(command, std::move(result));
    }
    playback.clear_control_cancellation();
    playback.stop();
    effective_state.store(PlaybackState::Stopped, std::memory_order_release);
    completed_changed.notify_all();
  }

  [[nodiscard]] AsyncPlaybackDiagnostics diagnostics() const {
    // Keep the versioned result and the underlying transport snapshot in one
    // publication interval. Without this lock spanning playback.diagnostics(),
    // a command could finish after the transport was sampled but before its
    // Succeeded result was copied, yielding an impossible combination such as
    // Succeeded Start with device_running == false. Callers use that pairing to
    // decide whether the audio clock may become authoritative.
    std::lock_guard result_lock(result_mutex);
    AsyncPlaybackDiagnostics result{
        .requested_state = requested_state.load(std::memory_order_acquire),
        .effective_state = effective_state.load(std::memory_order_acquire),
        .latest_requested_version = latest_requested_version.load(std::memory_order_acquire),
        .latest_completed_version = latest_completed_version.load(std::memory_order_acquire),
        .latest_published_version = latest_published_version.load(std::memory_order_acquire),
        .latest_result_version = 0,
        .latest_command = PlaybackCommandKind::Stop,
        .latest_status = PlaybackCommandStatus::Succeeded,
        .latest_error = std::nullopt,
        .shutting_down = shutting_down.load(std::memory_order_acquire),
        .playback = playback.diagnostics(),
    };
    result.latest_result_version = latest_result_version;
    result.latest_command = latest_command;
    result.latest_status = latest_status;
    result.latest_error = latest_error;
    return result;
  }

  RealtimeAudioPlayback playback;
  AsyncRealtimePlaybackConfiguration async_configuration;
  mutable std::mutex queue_mutex;
  std::condition_variable_any queue_changed;
  std::deque<QueuedCommand> commands;
  std::optional<QueuedCommand> active_command{};
  mutable std::mutex result_mutex;
  mutable std::condition_variable completed_changed;
  std::atomic<std::uint64_t> next_version{1};
  std::atomic<std::uint64_t> latest_requested_version{0};
  std::atomic<std::uint64_t> latest_completed_version{0};
  std::atomic<std::uint64_t> latest_published_version{0};
  std::atomic<PlaybackState> requested_state{PlaybackState::Stopped};
  std::atomic<PlaybackState> effective_state{PlaybackState::Stopped};
  std::atomic<bool> shutting_down{false};
  std::uint64_t latest_result_version{0};
  PlaybackCommandKind latest_command{PlaybackCommandKind::Stop};
  PlaybackCommandStatus latest_status{PlaybackCommandStatus::Succeeded};
  std::optional<PlaybackControlError> latest_error{};
  std::jthread control_thread;
};

AsyncRealtimeAudioPlayback::AsyncRealtimeAudioPlayback(
    std::shared_ptr<PlaybackAudioProvider> provider,
    const RealtimePlaybackConfiguration playback_configuration,
    std::unique_ptr<AudioOutputDevice> device,
    const AsyncRealtimePlaybackConfiguration async_configuration)
    : impl_(std::make_unique<Impl>(std::move(provider), playback_configuration, std::move(device),
                                   async_configuration)) {}

AsyncRealtimeAudioPlayback::~AsyncRealtimeAudioPlayback() = default;

PlaybackCommandReceipt
AsyncRealtimeAudioPlayback::request_start(const std::int64_t start_sample) noexcept {
  return impl_->enqueue(PlaybackCommandKind::Start, start_sample);
}

PlaybackCommandReceipt AsyncRealtimeAudioPlayback::request_pause() noexcept {
  return impl_->enqueue(PlaybackCommandKind::Pause, 0);
}

PlaybackCommandReceipt AsyncRealtimeAudioPlayback::request_resume() noexcept {
  return impl_->enqueue(PlaybackCommandKind::Resume, 0);
}

PlaybackCommandReceipt
AsyncRealtimeAudioPlayback::request_seek(const std::int64_t sample) noexcept {
  return impl_->enqueue(PlaybackCommandKind::Seek, sample);
}

PlaybackCommandReceipt AsyncRealtimeAudioPlayback::request_stop() noexcept {
  return impl_->enqueue(PlaybackCommandKind::Stop, 0);
}

PlaybackState AsyncRealtimeAudioPlayback::requested_state() const noexcept {
  return impl_->requested_state.load(std::memory_order_acquire);
}

PlaybackState AsyncRealtimeAudioPlayback::effective_state() const noexcept {
  return impl_->effective_state.load(std::memory_order_acquire);
}

std::uint64_t AsyncRealtimeAudioPlayback::latest_requested_version() const noexcept {
  return impl_->latest_requested_version.load(std::memory_order_acquire);
}

std::uint64_t AsyncRealtimeAudioPlayback::latest_completed_version() const noexcept {
  return impl_->latest_completed_version.load(std::memory_order_acquire);
}

std::int64_t AsyncRealtimeAudioPlayback::sample_counter() const noexcept {
  return impl_->playback.sample_counter();
}

std::int64_t AsyncRealtimeAudioPlayback::submitted_sample_counter() const noexcept {
  return impl_->playback.submitted_sample_counter();
}

bool AsyncRealtimeAudioPlayback::device_present() const noexcept {
  return impl_->playback.device_present();
}

bool AsyncRealtimeAudioPlayback::device_open() const noexcept {
  return impl_->playback.device_open();
}

std::size_t
AsyncRealtimeAudioPlayback::render_callback(const std::span<float> interleaved_output) noexcept {
  return impl_->playback.render_callback(interleaved_output);
}

AsyncPlaybackDiagnostics AsyncRealtimeAudioPlayback::diagnostics() const {
  return impl_->diagnostics();
}

PlaybackMeter::Reading AsyncRealtimeAudioPlayback::read_meter() const noexcept {
  return impl_->playback.read_meter();
}

bool AsyncRealtimeAudioPlayback::wait_until_completed(
    const std::uint64_t version, const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->result_mutex);
  return impl_->completed_changed.wait_for(lock, timeout, [this, version] {
    return impl_->latest_completed_version.load(std::memory_order_acquire) >= version ||
           impl_->shutting_down.load(std::memory_order_acquire);
  });
}

} // namespace video_editor::audio

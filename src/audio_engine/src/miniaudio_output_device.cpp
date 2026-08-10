// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/miniaudio_output_device.h"

#include "video_editor/audio_engine/realtime_playback.h"

#include <atomic>
#include <string>
#include <utility>

#if VIDEO_EDITOR_HAS_MINIAUDIO
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

static_assert(MA_VERSION_MAJOR == 0 && MA_VERSION_MINOR == 11 && MA_VERSION_REVISION == 25,
              "the audio backend requires pinned miniaudio 0.11.25");
#endif

namespace video_editor::audio {

class MiniaudioOutputDevice::Impl final {
public:
#if VIDEO_EDITOR_HAS_MINIAUDIO
  static void callback(ma_device* device, void* output, const void*,
                       const ma_uint32 frame_count) noexcept {
    auto* self = static_cast<Impl*>(device->pUserData);
    if (self == nullptr || output == nullptr || self->configuration.callback == nullptr) {
      return;
    }
    self->configuration.callback(self->configuration.user_data, static_cast<float*>(output),
                                 static_cast<std::size_t>(frame_count));
  }

  ma_device native_device{};
#endif
  AudioDeviceConfiguration configuration{};
  std::atomic<bool> open{false};
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> estimated_latency_frames{0};
};

MiniaudioOutputDevice::MiniaudioOutputDevice() : impl_(std::make_unique<Impl>()) {}

MiniaudioOutputDevice::~MiniaudioOutputDevice() {
  close();
}

MiniaudioOutputDevice::MiniaudioOutputDevice(MiniaudioOutputDevice&&) noexcept = default;
MiniaudioOutputDevice& MiniaudioOutputDevice::operator=(MiniaudioOutputDevice&& other) noexcept {
  if (this != &other) {
    close();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool MiniaudioOutputDevice::available() noexcept {
#if VIDEO_EDITOR_HAS_MINIAUDIO
  return true;
#else
  return false;
#endif
}

AudioDeviceResult MiniaudioOutputDevice::open(const AudioDeviceConfiguration& configuration) {
  if (configuration.callback == nullptr ||
      configuration.format.sample_rate != kPlaybackAudioFormat.sample_rate ||
      configuration.format.channels != kPlaybackAudioFormat.channels) {
    return AudioDeviceResult::failure(AudioDeviceErrorCode::InvalidConfiguration,
                                      "miniaudio requires 48 kHz stereo float32 playback");
  }
#if VIDEO_EDITOR_HAS_MINIAUDIO
  close();
  impl_->configuration = configuration;
  ma_device_config native_configuration = ma_device_config_init(ma_device_type_playback);
  native_configuration.playback.format = ma_format_f32;
  native_configuration.playback.channels = kPlaybackAudioFormat.channels;
  native_configuration.sampleRate = kPlaybackAudioFormat.sample_rate;
  native_configuration.dataCallback = &Impl::callback;
  native_configuration.pUserData = impl_.get();
  const ma_result result = ma_device_init(nullptr, &native_configuration, &impl_->native_device);
  if (result != MA_SUCCESS) {
    impl_->configuration = {};
    return AudioDeviceResult::failure(AudioDeviceErrorCode::OpenFailed,
                                      std::string{"could not open the default audio output: "} +
                                          ma_result_description(result));
  }
  impl_->open.store(true, std::memory_order_release);
  impl_->estimated_latency_frames.store(
      static_cast<std::uint64_t>(impl_->native_device.playback.internalPeriodSizeInFrames) *
          impl_->native_device.playback.internalPeriods,
      std::memory_order_release);
  return AudioDeviceResult::success();
#else
  static_cast<void>(configuration);
  return AudioDeviceResult::failure(
      AudioDeviceErrorCode::Unavailable,
      "miniaudio 0.11.25 was not available when this build was configured");
#endif
}

AudioDeviceResult MiniaudioOutputDevice::start() {
#if VIDEO_EDITOR_HAS_MINIAUDIO
  if (!impl_->open.load(std::memory_order_acquire)) {
    return AudioDeviceResult::failure(AudioDeviceErrorCode::StartFailed,
                                      "miniaudio output is not open");
  }
  if (impl_->running.load(std::memory_order_acquire)) {
    return AudioDeviceResult::success();
  }
  const ma_result result = ma_device_start(&impl_->native_device);
  if (result != MA_SUCCESS) {
    return AudioDeviceResult::failure(AudioDeviceErrorCode::StartFailed,
                                      std::string{"could not start the default audio output: "} +
                                          ma_result_description(result));
  }
  impl_->running.store(true, std::memory_order_release);
  return AudioDeviceResult::success();
#else
  return AudioDeviceResult::failure(AudioDeviceErrorCode::Unavailable,
                                    "miniaudio output is unavailable");
#endif
}

void MiniaudioOutputDevice::stop() noexcept {
#if VIDEO_EDITOR_HAS_MINIAUDIO
  if (impl_ != nullptr && impl_->open.load(std::memory_order_acquire) &&
      impl_->running.load(std::memory_order_acquire)) {
    static_cast<void>(ma_device_stop(&impl_->native_device));
    impl_->running.store(false, std::memory_order_release);
  }
#endif
}

void MiniaudioOutputDevice::close() noexcept {
  if (impl_ == nullptr) {
    return;
  }
#if VIDEO_EDITOR_HAS_MINIAUDIO
  if (impl_->open.load(std::memory_order_acquire)) {
    stop();
    ma_device_uninit(&impl_->native_device);
  }
#endif
  impl_->open.store(false, std::memory_order_release);
  impl_->running.store(false, std::memory_order_release);
  impl_->estimated_latency_frames.store(0U, std::memory_order_release);
  impl_->configuration = {};
}

bool MiniaudioOutputDevice::is_open() const noexcept {
  return impl_ != nullptr && impl_->open.load(std::memory_order_acquire);
}

bool MiniaudioOutputDevice::is_running() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

std::uint64_t MiniaudioOutputDevice::estimated_output_latency_frames() const noexcept {
  return impl_ != nullptr ? impl_->estimated_latency_frames.load(std::memory_order_acquire) : 0U;
}

} // namespace video_editor::audio

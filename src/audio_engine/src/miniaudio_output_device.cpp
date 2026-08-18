// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/miniaudio_output_device.h"

#include "video_editor/audio_engine/realtime_playback.h"

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if VIDEO_EDITOR_HAS_MINIAUDIO
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

static_assert(MA_VERSION_MAJOR == 0 && MA_VERSION_MINOR == 11 && MA_VERSION_REVISION == 25,
              "the audio backend requires pinned miniaudio 0.11.25");
#endif

namespace video_editor::audio {

#if VIDEO_EDITOR_HAS_MINIAUDIO
namespace {

[[nodiscard]] std::string encode_native_device_id(const ma_device_id& id) {
  // ma_device_id is a backend-defined union (not a pointer) and miniaudio
  // returns it fully initialized. Preserve the complete opaque value so IDs
  // remain independent of enumeration order; open() validates it against a
  // fresh context before passing it to the native device.
  static constexpr char digits[] = "0123456789abcdef";
  const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
  std::string result = "miniaudio:";
  result.reserve(result.size() + (sizeof(id) * 2U));
  for (std::size_t index = 0; index < sizeof(id); ++index) {
    result.push_back(digits[(bytes[index] >> 4U) & 0x0FU]);
    result.push_back(digits[bytes[index] & 0x0FU]);
  }
  return result;
}

[[nodiscard]] bool decode_native_device_id(std::string_view encoded, ma_device_id& id) noexcept {
  constexpr std::string_view prefix = "miniaudio:";
  if (!encoded.starts_with(prefix) || encoded.size() != prefix.size() + (sizeof(id) * 2U)) {
    return false;
  }
  auto nibble = [](const char value) noexcept -> int {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
      return value - 'A' + 10;
    return -1;
  };
  auto* bytes = reinterpret_cast<unsigned char*>(&id);
  for (std::size_t index = 0; index < sizeof(id); ++index) {
    const int high = nibble(encoded[prefix.size() + (index * 2U)]);
    const int low = nibble(encoded[prefix.size() + (index * 2U) + 1U]);
    if (high < 0 || low < 0)
      return false;
    bytes[index] = static_cast<unsigned char>((high << 4) | low);
  }
  return true;
}

} // namespace
#endif

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
  ma_context native_context{};
  bool context_initialized{false};
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

std::vector<AudioDeviceInfo> MiniaudioDeviceEnumerator::enumerate() {
#if VIDEO_EDITOR_HAS_MINIAUDIO
  ma_context context{};
  if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
    return {};
  }
  ma_device_info* devices = nullptr;
  ma_uint32 count = 0;
  const ma_result result = ma_context_get_devices(&context, &devices, &count, nullptr, nullptr);
  std::vector<AudioDeviceInfo> output;
  if (result == MA_SUCCESS) {
    output.reserve(count);
    for (ma_uint32 index = 0; index < count; ++index) {
      output.push_back({.id = encode_native_device_id(devices[index].id),
                        .name = devices[index].name,
                        .is_default = (devices[index].isDefault != 0),
                        .connected = true});
    }
  }
  ma_context_uninit(&context);
  return output;
#else
  return {};
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
  if (ma_context_init(nullptr, 0, nullptr, &impl_->native_context) != MA_SUCCESS) {
    impl_->configuration = {};
    return AudioDeviceResult::failure(AudioDeviceErrorCode::OpenFailed,
                                      "could not initialize the miniaudio device context");
  }
  impl_->context_initialized = true;
  ma_device_info* devices = nullptr;
  ma_uint32 device_count = 0;
  const bool enumerated =
      ma_context_get_devices(&impl_->native_context, &devices, &device_count, nullptr, nullptr) ==
      MA_SUCCESS;
  if (!enumerated && !configuration.device_id.empty()) {
    close();
    return AudioDeviceResult::failure(AudioDeviceErrorCode::Unavailable,
                                      "could not enumerate miniaudio playback devices");
  }
  const ma_device_id* selected_id_ptr = nullptr;
  if (!configuration.device_id.empty()) {
    ma_device_id requested_id{};
    if (!decode_native_device_id(configuration.device_id, requested_id)) {
      close();
      return AudioDeviceResult::failure(AudioDeviceErrorCode::Unavailable,
                                        "selected miniaudio device id is invalid");
    }
    for (ma_uint32 index = 0; index < device_count; ++index) {
      if (ma_device_id_equal(&requested_id, &devices[index].id)) {
        selected_id_ptr = &devices[index].id;
        break;
      }
    }
    if (selected_id_ptr == nullptr) {
      close();
      return AudioDeviceResult::failure(AudioDeviceErrorCode::Unavailable,
                                        "selected miniaudio device is not connected");
    }
  }
  ma_device_config native_configuration = ma_device_config_init(ma_device_type_playback);
  native_configuration.playback.format = ma_format_f32;
  native_configuration.playback.channels = kPlaybackAudioFormat.channels;
  native_configuration.playback.pDeviceID = selected_id_ptr;
  native_configuration.sampleRate = kPlaybackAudioFormat.sample_rate;
  native_configuration.dataCallback = &Impl::callback;
  native_configuration.pUserData = impl_.get();
  const ma_result result =
      ma_device_init(&impl_->native_context, &native_configuration, &impl_->native_device);
  if (result != MA_SUCCESS) {
    impl_->configuration = {};
    ma_context_uninit(&impl_->native_context);
    impl_->context_initialized = false;
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
  if (impl_->context_initialized) {
    ma_context_uninit(&impl_->native_context);
    impl_->context_initialized = false;
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

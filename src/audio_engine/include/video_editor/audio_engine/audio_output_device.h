// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace video_editor::audio {

using AudioDeviceCallback = void (*)(void* user_data, float* interleaved_output,
                                     std::size_t frame_count) noexcept;

struct AudioDeviceConfiguration final {
  AudioFormat format{};
  AudioDeviceCallback callback{nullptr};
  void* user_data{nullptr};
};

enum class AudioDeviceErrorCode : std::uint8_t {
  Unavailable,
  InvalidConfiguration,
  OpenFailed,
  StartFailed,
};

struct AudioDeviceError final {
  AudioDeviceErrorCode code{AudioDeviceErrorCode::Unavailable};
  std::string message;
};

struct AudioDeviceResult final {
  std::optional<AudioDeviceError> error{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !error.has_value();
  }
  [[nodiscard]] static AudioDeviceResult success() noexcept {
    return {};
  }
  [[nodiscard]] static AudioDeviceResult failure(AudioDeviceErrorCode code, std::string message) {
    return {.error = AudioDeviceError{.code = code, .message = std::move(message)}};
  }
};

// A device implementation must never invoke the callback before start(). stop()
// must synchronously prevent and join all callbacks before it returns. This lets
// the playback controller reset its SPSC ring safely on seek and stop. State
// queries must be safe while another control thread starts or stops the device.
class AudioOutputDevice {
public:
  virtual ~AudioOutputDevice() = default;

  [[nodiscard]] virtual AudioDeviceResult open(const AudioDeviceConfiguration& configuration) = 0;
  [[nodiscard]] virtual AudioDeviceResult start() = 0;
  virtual void stop() noexcept = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool is_open() const noexcept = 0;
  [[nodiscard]] virtual bool is_running() const noexcept = 0;
  // Conservative frames queued ahead of audible output. This query must be
  // callback-safe and lock-free; zero means the backend cannot estimate it.
  [[nodiscard]] virtual std::uint64_t estimated_output_latency_frames() const noexcept = 0;
};

} // namespace video_editor::audio

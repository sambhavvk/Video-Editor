// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_output_device.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::audio {

struct AudioDeviceInfo final {
  std::string id;
  std::string name;
  bool is_default{false};
  bool connected{true};
  friend bool operator==(const AudioDeviceInfo&, const AudioDeviceInfo&) = default;
};

enum class AudioDeviceChangeKind : std::uint8_t { Added, Removed, DefaultChanged };

struct AudioDeviceChange final {
  AudioDeviceChangeKind kind{AudioDeviceChangeKind::Added};
  AudioDeviceInfo device;
};

class AudioDeviceEnumerator {
public:
  virtual ~AudioDeviceEnumerator() = default;
  [[nodiscard]] virtual std::vector<AudioDeviceInfo> enumerate() = 0;
};

enum class AudioDeviceRecoveryCode : std::uint8_t {
  NoChange,
  Disconnected,
  Reopened,
  ReopenFailed,
  InvalidSelection,
};

struct AudioDeviceRecoveryResult final {
  AudioDeviceRecoveryCode code{AudioDeviceRecoveryCode::NoChange};
  std::optional<AudioDeviceError> error;
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return code != AudioDeviceRecoveryCode::ReopenFailed &&
           code != AudioDeviceRecoveryCode::InvalidSelection;
  }
};

// Non-realtime device selection and hot-plug coordinator. The enumerator or
// native backend calls refresh() from its own worker/event thread; this class
// never runs from an audio callback. It keeps the selected opaque id, stops a
// disconnected output synchronously, and reopens/restarts the same callback
// configuration when that id returns. Empty selection follows the backend
// default reported by the enumerator.
class AudioDeviceRecovery final {
public:
  explicit AudioDeviceRecovery(AudioOutputDevice& output) noexcept : output_(output) {}

  AudioDeviceRecovery(const AudioDeviceRecovery&) = delete;
  AudioDeviceRecovery& operator=(const AudioDeviceRecovery&) = delete;

  [[nodiscard]] AudioDeviceRecoveryResult select(std::string device_id,
                                                 AudioDeviceConfiguration configuration);
  [[nodiscard]] AudioDeviceRecoveryResult refresh(std::span<const AudioDeviceInfo> devices);
  [[nodiscard]] const std::string& selected_id() const noexcept {
    return selected_id_;
  }
  [[nodiscard]] std::span<const AudioDeviceInfo> devices() const noexcept {
    return devices_;
  }
  [[nodiscard]] bool was_running() const noexcept {
    return was_running_;
  }

private:
  AudioOutputDevice& output_;
  std::string selected_id_;
  std::string active_id_;
  AudioDeviceConfiguration configuration_{};
  std::vector<AudioDeviceInfo> devices_;
  bool was_running_{false};
};

} // namespace video_editor::audio

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/audio_device_manager.h"

#include <algorithm>
#include <utility>

namespace video_editor::audio {
namespace {

[[nodiscard]] const AudioDeviceInfo* find_device(std::span<const AudioDeviceInfo> devices,
                                                 std::string_view id) noexcept {
  const auto found = std::find_if(devices.begin(), devices.end(),
                                  [id](const AudioDeviceInfo& device) { return device.id == id; });
  return found == devices.end() ? nullptr : &*found;
}

} // namespace

AudioDeviceRecoveryResult AudioDeviceRecovery::select(std::string device_id,
                                                      AudioDeviceConfiguration configuration) {
  if (configuration.callback == nullptr) {
    return {.code = AudioDeviceRecoveryCode::InvalidSelection,
            .error = AudioDeviceError{.code = AudioDeviceErrorCode::InvalidConfiguration,
                                      .message = "audio device callback is required"},
            .message = "audio device callback is required"};
  }
  const AudioDeviceInfo* selected = nullptr;
  if (device_id.empty()) {
    const auto found =
        std::find_if(devices_.begin(), devices_.end(), [](const AudioDeviceInfo& device) {
          return device.is_default && device.connected;
        });
    selected = found == devices_.end() ? nullptr : &*found;
  } else {
    selected = find_device(devices_, device_id);
  }
  if (selected == nullptr) {
    return {.code = AudioDeviceRecoveryCode::InvalidSelection,
            .error = AudioDeviceError{.code = AudioDeviceErrorCode::Unavailable,
                                      .message = "selected audio device is not connected"},
            .message = "selected audio device is not connected"};
  }
  if (output_.is_open()) {
    was_running_ = output_.is_running();
    output_.stop();
    output_.close();
  }
  selected_id_ = std::move(device_id);
  active_id_ = selected->id;
  configuration_ = std::move(configuration);
  configuration_.device_id = selected_id_;
  const auto opened = output_.open(configuration_);
  if (!opened) {
    return {.code = AudioDeviceRecoveryCode::ReopenFailed,
            .error = opened.error,
            .message = opened.error ? opened.error->message : "could not open audio device"};
  }
  if (was_running_) {
    const auto started = output_.start();
    if (!started) {
      output_.close();
      return {.code = AudioDeviceRecoveryCode::ReopenFailed,
              .error = started.error,
              .message = started.error ? started.error->message : "could not start audio device"};
    }
  }
  return {.code = AudioDeviceRecoveryCode::Reopened,
          .error = std::nullopt,
          .message = "audio device selected"};
}

AudioDeviceRecoveryResult
AudioDeviceRecovery::refresh(const std::span<const AudioDeviceInfo> devices) {
  devices_.assign(devices.begin(), devices.end());
  const AudioDeviceInfo* selected = nullptr;
  if (selected_id_.empty()) {
    const auto found =
        std::find_if(devices_.begin(), devices_.end(), [](const AudioDeviceInfo& device) {
          return device.is_default && device.connected;
        });
    selected = found == devices_.end() ? nullptr : &*found;
  } else {
    selected = find_device(devices_, selected_id_);
  }
  const bool connected = selected != nullptr && selected->connected;
  if (!connected) {
    if (output_.is_open()) {
      was_running_ = output_.is_running();
      output_.stop();
      output_.close();
      return {.code = AudioDeviceRecoveryCode::Disconnected,
              .error = std::nullopt,
              .message = "selected audio device disconnected"};
    }
    return {.code = AudioDeviceRecoveryCode::NoChange,
            .error = std::nullopt,
            .message = "selected audio device remains disconnected"};
  }
  if (output_.is_open() && selected->id != active_id_) {
    was_running_ = output_.is_running();
    output_.stop();
    output_.close();
    active_id_ = selected->id;
    configuration_.device_id = selected_id_.empty() ? std::string{} : selected_id_;
    const auto opened = output_.open(configuration_);
    if (!opened) {
      return {.code = AudioDeviceRecoveryCode::ReopenFailed,
              .error = opened.error,
              .message =
                  opened.error ? opened.error->message : "could not switch default audio device"};
    }
    if (was_running_) {
      const auto started = output_.start();
      if (!started) {
        output_.close();
        return {.code = AudioDeviceRecoveryCode::ReopenFailed,
                .error = started.error,
                .message = started.error ? started.error->message
                                         : "could not restart default audio device"};
      }
    }
    return {.code = AudioDeviceRecoveryCode::Reopened,
            .error = std::nullopt,
            .message = "default audio device changed"};
  }
  if (!output_.is_open() && configuration_.callback != nullptr) {
    active_id_ = selected->id;
    const auto opened = output_.open(configuration_);
    if (!opened) {
      return {.code = AudioDeviceRecoveryCode::ReopenFailed,
              .error = opened.error,
              .message = opened.error ? opened.error->message : "could not reopen audio device"};
    }
    if (was_running_) {
      const auto started = output_.start();
      if (!started) {
        output_.close();
        return {.code = AudioDeviceRecoveryCode::ReopenFailed,
                .error = started.error,
                .message =
                    started.error ? started.error->message : "could not restart audio device"};
      }
    }
    return {.code = AudioDeviceRecoveryCode::Reopened,
            .error = std::nullopt,
            .message = "audio device recovered"};
  }
  return {.code = AudioDeviceRecoveryCode::NoChange,
          .error = std::nullopt,
          .message = "audio device state unchanged"};
}

} // namespace video_editor::audio

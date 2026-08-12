// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_output_device.h"

#include <memory>

namespace video_editor::audio {

// Optional native backend. The class remains available in no-device builds and
// reports Unavailable from open(); available() identifies builds compiled with
// the pinned miniaudio header.
class MiniaudioOutputDevice final : public AudioOutputDevice {
public:
  MiniaudioOutputDevice();
  ~MiniaudioOutputDevice() override;

  MiniaudioOutputDevice(const MiniaudioOutputDevice&) = delete;
  MiniaudioOutputDevice& operator=(const MiniaudioOutputDevice&) = delete;
  MiniaudioOutputDevice(MiniaudioOutputDevice&&) noexcept;
  MiniaudioOutputDevice& operator=(MiniaudioOutputDevice&&) noexcept;

  [[nodiscard]] static bool available() noexcept;
  [[nodiscard]] AudioDeviceResult open(const AudioDeviceConfiguration& configuration) override;
  [[nodiscard]] AudioDeviceResult start() override;
  void stop() noexcept override;
  void close() noexcept override;
  [[nodiscard]] bool is_open() const noexcept override;
  [[nodiscard]] bool is_running() const noexcept override;
  [[nodiscard]] std::uint64_t estimated_output_latency_frames() const noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio

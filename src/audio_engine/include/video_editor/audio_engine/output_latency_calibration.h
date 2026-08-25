// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_output_device.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace video_editor::audio {

struct OutputLatencyMeasurementOptions final {
  // Stable opaque output-device identifier. Empty selects the backend default.
  std::string device_id;
  std::chrono::milliseconds timeout{2'000};
  std::size_t minimum_callbacks{1};
};

// Measures output latency by opening the device, running the realtime callback
// path with silence, and reading the backend's estimated_output_latency_frames()
// after the first callback period completes. For FakeAudioDevice tests, pump the
// device while this call is blocked so callbacks can run.
[[nodiscard]] std::optional<std::uint64_t>
measure_output_latency_frames(AudioOutputDevice& device,
                              const OutputLatencyMeasurementOptions& options = {});

} // namespace video_editor::audio

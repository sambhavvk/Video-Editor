// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/time.h"

#include <cstddef>
#include <optional>
#include <span>

namespace video_editor::audio_render {

struct MulticamWaveformMatch final {
  edit::Time offset{};
  double confidence{0.0};
};

struct MulticamWaveformSyncOptions final {
  std::size_t max_lag_samples{48'000};
  double minimum_confidence{0.35};
};

[[nodiscard]] std::optional<MulticamWaveformMatch>
matchWaveformOffset(std::span<const float> reference, std::span<const float> candidate,
                    int sample_rate, MulticamWaveformSyncOptions options = {});

} // namespace video_editor::audio_render

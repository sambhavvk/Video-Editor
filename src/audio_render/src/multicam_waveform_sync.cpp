// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/multicam_waveform_sync.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace video_editor::audio_render {
namespace {

[[nodiscard]] double correlation_at_lag(const std::span<const float> reference,
                                      const std::span<const float> candidate,
                                      const std::size_t lag) {
  const std::size_t overlap = std::min(reference.size(), candidate.size()) - lag;
  if (overlap < 256) {
    return 0.0;
  }
  double numerator = 0.0;
  double ref_energy = 0.0;
  double cand_energy = 0.0;
  for (std::size_t index = 0; index < overlap; ++index) {
    const double ref = reference[index];
    const double cand = candidate[index + lag];
    numerator += ref * cand;
    ref_energy += ref * ref;
    cand_energy += cand * cand;
  }
  if (ref_energy <= 0.0 || cand_energy <= 0.0) {
    return 0.0;
  }
  return numerator / std::sqrt(ref_energy * cand_energy);
}

} // namespace

std::optional<MulticamWaveformMatch>
matchWaveformOffset(const std::span<const float> reference, const std::span<const float> candidate,
                    const int sample_rate, MulticamWaveformSyncOptions options) {
  if (reference.empty() || candidate.empty() || sample_rate <= 0) {
    return std::nullopt;
  }
  const std::size_t max_lag = std::min(
      options.max_lag_samples,
      candidate.size() > reference.size() ? candidate.size() - reference.size() : std::size_t{0});
  double best_score = 0.0;
  std::size_t best_lag = 0;
  for (std::size_t lag = 0; lag <= max_lag; ++lag) {
    const double score = correlation_at_lag(reference, candidate, lag);
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  if (best_score < options.minimum_confidence) {
    return std::nullopt;
  }
  MulticamWaveformMatch result;
  result.confidence = best_score;
  result.offset = edit::Time(static_cast<std::int64_t>(best_lag), static_cast<std::uint32_t>(sample_rate));
  return result;
}

} // namespace video_editor::audio_render

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/multicam_waveform_sync.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace video_editor::audio_render {
namespace {

[[nodiscard]] double correlation_at_lag(const std::span<const float> reference,
                                        const std::span<const float> candidate,
                                        const std::ptrdiff_t lag,
                                        const std::size_t min_overlap) {
  std::size_t ref_offset = 0;
  std::size_t cand_offset = 0;
  if (lag >= 0) {
    cand_offset = static_cast<std::size_t>(lag);
  } else {
    ref_offset = static_cast<std::size_t>(-lag);
  }
  if (ref_offset >= reference.size() || cand_offset >= candidate.size()) {
    return 0.0;
  }
  const std::size_t overlap =
      std::min(reference.size() - ref_offset, candidate.size() - cand_offset);
  if (overlap < min_overlap) {
    return 0.0;
  }
  double numerator = 0.0;
  double ref_energy = 0.0;
  double cand_energy = 0.0;
  for (std::size_t index = 0; index < overlap; ++index) {
    const double ref = reference[index + ref_offset];
    const double cand = candidate[index + cand_offset];
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
  const std::size_t min_len = std::min(reference.size(), candidate.size());
  if (min_len < 8) {
    return std::nullopt;
  }
  const std::size_t min_overlap =
      std::max<std::size_t>(8, std::min<std::size_t>(256, min_len / 4));
  if (min_overlap > min_len) {
    return std::nullopt;
  }
  const std::size_t max_feasible = min_len - min_overlap;
  const std::size_t max_lag = std::min(options.max_lag_samples, max_feasible);
  double best_score = 0.0;
  std::ptrdiff_t best_lag = 0;
  const auto max_lag_signed = static_cast<std::ptrdiff_t>(max_lag);
  for (std::ptrdiff_t lag = -max_lag_signed; lag <= max_lag_signed; ++lag) {
    const double score = correlation_at_lag(reference, candidate, lag, min_overlap);
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  if (best_score < options.minimum_confidence) {
    return std::nullopt;
  }
  MulticamWaveformMatch result;
  result.lag_samples = static_cast<std::int64_t>(best_lag);
  result.confidence = best_score;
  result.offset =
      edit::Time(result.lag_samples, static_cast<std::uint32_t>(sample_rate));
  return result;
}

} // namespace video_editor::audio_render

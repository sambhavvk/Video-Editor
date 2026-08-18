// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/loudness_normalize.h"

#include "video_editor/audio_engine/loudness_meter.h"
#include "video_editor/media_codec/format_open.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace video_editor::audio_render {
namespace {

constexpr std::size_t kNormalizationBlockFrames = 5U * kTimelineAudioSampleRate;

[[nodiscard]] LoudnessNormalizeOutcome failure(std::string message) {
  return LoudnessNormalizeOutcome::failure({.message = std::move(message)});
}

} // namespace

LoudnessNormalizeOutcome compute_normalization_gain(
    const edit::TimelineSnapshot& snapshot,
    std::shared_ptr<const OriginalAudioProvider> originals,
    const double target_lufs, const std::stop_token cancellation) {
  media::install_quiet_ffmpeg_log_filter();
  const std::int64_t total_samples =
      snapshot.duration().rescaledTo(kTimelineAudioSampleRate, edit::RoundingMode::Ceil).value();
  if (total_samples <= 0) {
    return failure("cannot normalize an empty timeline");
  }

  TimelineAudioRenderer renderer(std::move(originals));
  audio::LoudnessMeter meter(
      {.sample_rate = kTimelineAudioSampleRate, .channels = kTimelineAudioChannels});
  if (!meter.valid()) {
    return failure("could not initialize the loudness meter");
  }

  for (std::int64_t start_sample = 0; start_sample < total_samples;) {
    if (cancellation.stop_requested()) {
      return failure("audio rendering failed: loudness analysis was cancelled");
    }
    const auto remaining = total_samples - start_sample;
    const auto sample_count = static_cast<std::size_t>(
        std::min<std::int64_t>(remaining, static_cast<std::int64_t>(kNormalizationBlockFrames)));
    const auto rendered = renderer.render(
        snapshot, {.start_sample = start_sample,
                   .sample_count = sample_count,
                   .cancellation = cancellation});
    if (!rendered) {
      return failure("audio rendering failed: " + rendered.error().message);
    }
    if (!meter.add(rendered.value())) {
      return failure("could not measure rendered audio");
    }
    start_sample += static_cast<std::int64_t>(sample_count);
  }

  const auto reading = meter.reading();
  if (!reading.integrated_lufs.has_value() || !std::isfinite(*reading.integrated_lufs)) {
    return failure("cannot normalize a silent timeline");
  }

  const double gain_db = target_lufs - *reading.integrated_lufs;
  if (!std::isfinite(gain_db)) {
    return failure("normalization target is not finite");
  }
  return LoudnessNormalizeOutcome::success({.integrated_lufs = *reading.integrated_lufs,
                                            .gain_db = gain_db});
}

} // namespace video_editor::audio_render

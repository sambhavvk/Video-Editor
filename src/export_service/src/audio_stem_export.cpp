// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/audio_stem_export.h"

#include "video_editor/edit_model/model.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

#include <array>
#include <fstream>
#include <vector>

namespace video_editor::export_service {
namespace {

[[nodiscard]] bool write_pcm_wav(const std::filesystem::path& path, std::span<const float> interleaved,
                                   const std::uint32_t channels, const std::uint32_t sample_rate) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  const auto sample_count = interleaved.size() / channels;
  const std::uint32_t data_size = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
  out.write("RIFF", 4);
  const std::uint32_t riff_size = 36U + data_size;
  std::array<char, 4> size_bytes{};
  AV_WL32(size_bytes.data(), riff_size);
  out.write(size_bytes.data(), 4);
  out.write("WAVEfmt ", 8);
  AV_WL32(size_bytes.data(), 16U);
  out.write(size_bytes.data(), 4);
  AV_WL16(size_bytes.data(), 3U);
  out.write(size_bytes.data(), 2);
  AV_WL16(size_bytes.data(), channels);
  out.write(size_bytes.data(), 2);
  AV_WL32(size_bytes.data(), sample_rate);
  out.write(size_bytes.data(), 4);
  const std::uint32_t byte_rate = sample_rate * channels * sizeof(float);
  AV_WL32(size_bytes.data(), byte_rate);
  out.write(size_bytes.data(), 4);
  const std::uint16_t block_align = static_cast<std::uint16_t>(channels * sizeof(float));
  AV_WL16(size_bytes.data(), block_align);
  out.write(size_bytes.data(), 2);
  AV_WL16(size_bytes.data(), 32U);
  out.write(size_bytes.data(), 2);
  out.write("data", 4);
  AV_WL32(size_bytes.data(), data_size);
  out.write(size_bytes.data(), 4);
  out.write(reinterpret_cast<const char*>(interleaved.data()),
            static_cast<std::streamsize>(interleaved.size() * sizeof(float)));
  return static_cast<bool>(out);
}

} // namespace

edit::Result<AudioStemExportResult, std::string>
export_production_audio_stems(const AudioStemExportRequest& request) {
  if (!request.audio_renderer || request.clip_ids.empty() || request.destination_directory.empty()) {
    return edit::Result<AudioStemExportResult, std::string>::failure("invalid audio stem request");
  }
  const edit::Sequence& sequence = request.snapshot.sequence();
  const edit::Project& project = request.snapshot.project();

  AudioStemExportResult result;
  for (const edit::EntityId& clip_id : request.clip_ids) {
    if (request.cancellation.stop_requested()) {
      return edit::Result<AudioStemExportResult, std::string>::failure("cancelled");
    }
    const edit::Clip* clip = edit::findClip(sequence, clip_id);
    if (clip == nullptr || clip->kind != edit::ClipKind::Audio) {
      return edit::Result<AudioStemExportResult, std::string>::failure("audio clip was not found");
    }
    const edit::Asset* asset = edit::findAsset(project, clip->asset_id);
    if (asset == nullptr) {
      return edit::Result<AudioStemExportResult, std::string>::failure("stem source asset missing");
    }

    edit::TimeRange export_range = request.timeline_range;
    if (export_range.duration.isZero()) {
      export_range = clip->timeline_range;
    }
    const edit::Time requested_before = request.handle_before;
    const edit::Time available_before = clip->source_range.start;
    const edit::Time applied_before =
        requested_before <= available_before ? requested_before : available_before;
    AudioStemFileResult stem;
    stem.clip_id = clip_id;
    if (applied_before < requested_before) {
      stem.warnings.push_back("requested head handle trimmed to available media");
    }
    const edit::Time tail_available = asset->duration - clip->source_range.end();
    const edit::Time applied_after =
        request.handle_after <= tail_available ? request.handle_after : tail_available;
    if (applied_after < request.handle_after) {
      stem.warnings.push_back("requested tail handle trimmed to available media");
    }

    const edit::TimeRange render_range{
        export_range.start - applied_before,
        export_range.duration + applied_before + applied_after};
    const std::int64_t sample_count =
        render_range.duration.rescaledTo(audio_render::kTimelineAudioSampleRate,
                                           edit::RoundingMode::Ceil)
            .value();
    if (sample_count <= 0) {
      continue;
    }
    audio_render::AudioRenderRequest audio_request;
    audio_request.start_sample = render_range.start.rescaledTo(
        audio_render::kTimelineAudioSampleRate, edit::RoundingMode::Floor).value();
    audio_request.sample_count = static_cast<std::size_t>(sample_count);
    audio_request.cancellation = request.cancellation;
    const auto rendered = request.audio_renderer->render(request.snapshot, audio_request);
    if (!rendered) {
      return edit::Result<AudioStemExportResult, std::string>::failure(rendered.error().message);
    }

    const std::vector<float> interleaved = rendered.value().interleaved();
    stem.sample_count = static_cast<std::uint64_t>(sample_count);
    stem.source_start_reference = clip->source_range.start - applied_before;
    stem.path = request.destination_directory /
                (clip->name.empty() ? clip_id.toString() + ".wav" : clip->name + ".wav");
    if (!write_pcm_wav(stem.path, interleaved, audio_render::kTimelineAudioChannels,
                       audio_render::kTimelineAudioSampleRate)) {
      return edit::Result<AudioStemExportResult, std::string>::failure("could not write stem wav");
    }
    result.stems.push_back(std::move(stem));
  }
  return edit::Result<AudioStemExportResult, std::string>::success(std::move(result));
}

} // namespace video_editor::export_service

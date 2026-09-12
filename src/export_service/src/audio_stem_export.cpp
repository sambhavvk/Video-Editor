// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/audio_stem_export.h"

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/timeline_editor.h"

extern "C" {
#include <libavutil/intreadwrite.h>
}

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace video_editor::export_service {
namespace {

[[nodiscard]] std::string sanitize_stem_filename(std::string name) {
  for (char& character : name) {
    if (character == '/' || character == '\\' || character == '\0' || character == ':') {
      character = '_';
    }
  }
  return name.empty() ? "stem" : name;
}

[[nodiscard]] bool write_pcm_wav(const std::filesystem::path& path, std::span<const float> interleaved,
                                   const std::uint32_t channels, const std::uint32_t sample_rate) {
  if (channels == 0U || interleaved.size() % channels != 0U) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
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
  AV_WL16(size_bytes.data(), static_cast<std::uint16_t>(channels));
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

[[nodiscard]] edit::TimeRange used_source_range(const edit::Clip& clip,
                                                const edit::TimeRange& export_range) {
  if (export_range.duration.isZero() || clip.reversed ||
      clip.playback_rate != edit::Rate(1, 1)) {
    return clip.source_range;
  }
  const edit::TimeRange overlap = clip.timeline_range.intersection(export_range);
  if (overlap.empty()) {
    return {};
  }
  const edit::Time head = overlap.start - clip.timeline_range.start;
  return edit::TimeRange{clip.source_range.start + head, overlap.duration};
}

[[nodiscard]] edit::Result<edit::TimelineSnapshot, std::string> make_isolated_stem_snapshot(
    const edit::Project& project, const edit::Sequence& sequence, const edit::Clip& clip,
    const edit::TimeRange& source_range) {
  const edit::Asset* asset = edit::findAsset(project, clip.asset_id);
  if (asset == nullptr) {
    return edit::Result<edit::TimelineSnapshot, std::string>::failure("stem source asset missing");
  }
  edit::Project isolated;
  isolated.assets.push_back(*asset);
  edit::Sequence stem_sequence;
  stem_sequence.name = "stem";
  stem_sequence.frame_rate = sequence.frame_rate;
  stem_sequence.width = sequence.width == 0U ? 1920U : sequence.width;
  stem_sequence.height = sequence.height == 0U ? 1080U : sequence.height;
  stem_sequence.audio_sample_rate = audio_render::kTimelineAudioSampleRate;
  edit::Track track;
  track.kind = edit::TrackKind::Audio;
  edit::Clip stem_clip = clip;
  stem_clip.playback_rate = edit::Rate(1, 1);
  stem_clip.reversed = false;
  stem_clip.fade_in = {};
  stem_clip.fade_out = {};
  stem_clip.effects.clear();
  stem_clip.enabled = true;
  stem_clip.source_range = source_range;
  stem_clip.timeline_range = edit::TimeRange{edit::Time{}, source_range.duration};
  track.clips.push_back(std::move(stem_clip));
  stem_sequence.tracks.push_back(std::move(track));
  isolated.sequences.push_back(std::move(stem_sequence));
  try {
    edit::TimelineEditor editor(std::move(isolated));
    const auto stored = editor.projectAt(editor.revision());
    if (stored == nullptr || stored->sequences.empty()) {
      return edit::Result<edit::TimelineSnapshot, std::string>::failure(
          "could not isolate stem sequence");
    }
    auto snapshot = editor.snapshot(stored->sequences.front().id, editor.revision());
    if (!snapshot) {
      return edit::Result<edit::TimelineSnapshot, std::string>::failure(snapshot.error().message);
    }
    return edit::Result<edit::TimelineSnapshot, std::string>::success(std::move(snapshot).value());
  } catch (const std::invalid_argument& exception) {
    return edit::Result<edit::TimelineSnapshot, std::string>::failure(exception.what());
  }
}

void remove_written_stems(const std::vector<std::filesystem::path>& paths) {
  std::error_code error;
  for (const auto& path : paths) {
    std::filesystem::remove(path, error);
  }
}

} // namespace

edit::Result<AudioStemExportResult, std::string>
export_production_audio_stems(const AudioStemExportRequest& request) {
  if (!request.audio_renderer || request.clip_ids.empty() || request.destination_directory.empty()) {
    return edit::Result<AudioStemExportResult, std::string>::failure("invalid audio stem request");
  }
  const edit::Sequence& sequence = request.snapshot.sequence();
  const edit::Project& project = request.snapshot.project();

  std::error_code directory_error;
  std::filesystem::create_directories(request.destination_directory, directory_error);
  if (directory_error) {
    return edit::Result<AudioStemExportResult, std::string>::failure(
        "could not create stem export directory");
  }

  AudioStemExportResult result;
  std::vector<std::filesystem::path> written;
  for (const edit::EntityId& clip_id : request.clip_ids) {
    if (request.cancellation.stop_requested()) {
      remove_written_stems(written);
      return edit::Result<AudioStemExportResult, std::string>::failure("cancelled");
    }
    const edit::Clip* clip = edit::findClip(sequence, clip_id);
    if (clip == nullptr || clip->kind != edit::ClipKind::Audio) {
      remove_written_stems(written);
      return edit::Result<AudioStemExportResult, std::string>::failure("audio clip was not found");
    }
    const edit::Asset* asset = edit::findAsset(project, clip->asset_id);
    if (asset == nullptr) {
      remove_written_stems(written);
      return edit::Result<AudioStemExportResult, std::string>::failure("stem source asset missing");
    }

    edit::TimeRange export_range = request.timeline_range;
    if (export_range.duration.isZero()) {
      export_range = clip->timeline_range;
    }
    if (clip->timeline_range.intersection(export_range).empty()) {
      continue;
    }
    const edit::TimeRange used_source = used_source_range(*clip, export_range);
    if (used_source.duration.isZero()) {
      continue;
    }

    const edit::Time requested_before = request.handle_before;
    const edit::Time available_before = used_source.start.isNegative() ? edit::Time{} : used_source.start;
    const edit::Time applied_before =
        requested_before <= available_before ? requested_before : available_before;
    AudioStemFileResult stem;
    stem.clip_id = clip_id;
    if (applied_before < requested_before) {
      stem.warnings.push_back("requested head handle trimmed to available media");
    }
    edit::Time tail_available{};
    if (asset->duration > used_source.end()) {
      tail_available = asset->duration - used_source.end();
    }
    const edit::Time applied_after =
        request.handle_after <= tail_available ? request.handle_after : tail_available;
    if (applied_after < request.handle_after) {
      stem.warnings.push_back("requested tail handle trimmed to available media");
    }

    const edit::TimeRange render_source{used_source.start - applied_before,
                                        used_source.duration + applied_before + applied_after};
    if (render_source.duration.isZero() || render_source.duration.isNegative() ||
        render_source.start.isNegative()) {
      continue;
    }
    const std::int64_t sample_count =
        render_source.duration
            .rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::Ceil)
            .value();
    if (sample_count <= 0) {
      continue;
    }

    auto isolated = make_isolated_stem_snapshot(project, sequence, *clip, render_source);
    if (!isolated) {
      remove_written_stems(written);
      return edit::Result<AudioStemExportResult, std::string>::failure(isolated.error());
    }

    audio_render::AudioRenderRequest audio_request;
    audio_request.start_sample = 0;
    audio_request.sample_count = static_cast<std::size_t>(sample_count);
    audio_request.cancellation = request.cancellation;
    const auto rendered = request.audio_renderer->render(isolated.value(), audio_request);
    if (!rendered) {
      remove_written_stems(written);
      if (request.cancellation.stop_requested() ||
          rendered.error().code == audio_render::AudioRenderErrorCode::Cancelled) {
        return edit::Result<AudioStemExportResult, std::string>::failure("cancelled");
      }
      return edit::Result<AudioStemExportResult, std::string>::failure(rendered.error().message);
    }

    const std::vector<float> interleaved = rendered.value().interleaved();
    stem.sample_count = static_cast<std::uint64_t>(sample_count);
    stem.source_start_reference = render_source.start;
    const std::string file_stem =
        sanitize_stem_filename(clip->name.empty() ? clip_id.toString() : clip->name);
    stem.path = request.destination_directory / (file_stem + ".wav");
    if (!write_pcm_wav(stem.path, interleaved, audio_render::kTimelineAudioChannels,
                       audio_render::kTimelineAudioSampleRate)) {
      std::error_code error;
      std::filesystem::remove(stem.path, error);
      remove_written_stems(written);
      return edit::Result<AudioStemExportResult, std::string>::failure("could not write stem wav");
    }
    written.push_back(stem.path);
    result.stems.push_back(std::move(stem));
  }
  return edit::Result<AudioStemExportResult, std::string>::success(std::move(result));
}

} // namespace video_editor::export_service

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/embedded_extract.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] edit::Time microsecondsToTime(const std::int64_t microseconds) {
  if (microseconds <= 0) {
    return edit::Time(0, 1'000);
  }
  return edit::Time(microseconds / 1'000, 1'000);
}

[[nodiscard]] edit::TimeRange cueRange(const media::ExtractedSubtitleCue& cue) {
  const edit::Time start = microsecondsToTime(cue.start_microseconds);
  const edit::Time end = microsecondsToTime(cue.end_microseconds);
  if (end <= start) {
    return edit::TimeRange(start, edit::Time(0, 1'000));
  }
  return edit::TimeRange(start, end - start);
}

} // namespace

std::string embeddedCaptionProvenance(const std::string& asset_id, const int stream_index) {
  return asset_id + ":" + std::to_string(stream_index);
}

SubtitleFormat subtitleFormatForCodec(const std::string_view codec_name) noexcept {
  const std::string lowered(codec_name);
  if (lowered == "webvtt" || lowered == "vtt") {
    return SubtitleFormat::WebVtt;
  }
  return SubtitleFormat::Srt;
}

EmbeddedSubtitleResult importEmbeddedSubtitles(const std::filesystem::path& uri,
                                               const media::AssetDescriptor& asset,
                                               const EmbeddedSubtitleImportOptions& options) {
  if (options.asset_id.empty()) {
    return EmbeddedSubtitleResult::failure(
        {.code = media::MediaErrorCode::InvalidArgument, .message = "asset id is empty"});
  }
  if (options.stream_index < 0) {
    return EmbeddedSubtitleResult::failure(
        {.code = media::MediaErrorCode::InvalidArgument, .message = "subtitle stream index is invalid"});
  }

  const auto streams = media::list_subtitle_streams(asset);
  const auto stream_it = std::ranges::find_if(
      streams, [&](const media::SubtitleStreamInfo& stream) {
        return stream.index == options.stream_index;
      });
  if (stream_it == streams.end()) {
    return EmbeddedSubtitleResult::failure(
        {.code = media::MediaErrorCode::InvalidArgument,
         .message = "asset does not contain the requested subtitle stream"});
  }
  if (!stream_it->supported_text_codec) {
    return EmbeddedSubtitleResult::failure(
        {.code = media::MediaErrorCode::Unsupported,
         .message = media::is_bitmap_subtitle_codec(stream_it->codec_name)
                        ? "bitmap or styled subtitle streams are not supported: " +
                              stream_it->codec_name
                        : "subtitle stream codec is not supported for text extraction: " +
                              stream_it->codec_name});
  }

  auto extracted = media::extract_text_subtitles(uri, options.stream_index);
  if (!extracted) {
    return EmbeddedSubtitleResult::failure(
        {.code = extracted.error().code, .message = extracted.error().message});
  }

  CaptionDocument document;
  document.format = subtitleFormatForCodec(extracted.value().codec_name);
  document.language = extracted.value().language.empty() ? stream_it->language
                                                         : extracted.value().language;
  document.cues.reserve(extracted.value().cues.size());

  const edit::CaptionProvenance provenance{
      .source = edit::CaptionWordSource::Imported,
      .model_identity = embeddedCaptionProvenance(options.asset_id, options.stream_index),
  };

  for (const media::ExtractedSubtitleCue& cue : extracted.value().cues) {
    CaptionCue caption_cue;
    caption_cue.range = cueRange(cue);
    caption_cue.text = cue.text;
    caption_cue.provenance = provenance;
    document.cues.push_back(std::move(caption_cue));
  }

  return EmbeddedSubtitleResult::success(std::move(document));
}

std::vector<edit::Caption> toEditCaptionsFromEmbedded(const CaptionDocument& document) {
  return toEditCaptions(document);
}

} // namespace video_editor::caption_service

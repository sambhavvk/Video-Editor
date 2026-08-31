// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/subtitle_extract.h"

#include "video_editor/media_codec/format_open.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

namespace video_editor::media {
namespace {

struct FormatContextCloser {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      avformat_close_input(&context);
    }
  }
};

struct CodecContextCloser {
  void operator()(AVCodecContext* context) const noexcept {
    if (context != nullptr) {
      avcodec_free_context(&context);
    }
  }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextCloser>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextCloser>;

std::string ffmpeg_error_string(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return buffer.data();
}

MediaError make_error(const MediaErrorCode code, const int native_code, std::string message) {
  if (native_code < 0) {
    message += ": " + ffmpeg_error_string(native_code);
  }
  return {.code = code, .native_code = native_code, .message = std::move(message)};
}

std::string to_lower_ascii(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char character : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return lowered;
}

[[nodiscard]] bool codec_alias_matches(std::string_view codec_name,
                                       std::initializer_list<std::string_view> aliases) noexcept {
  const std::string lowered = to_lower_ascii(codec_name);
  for (const std::string_view alias : aliases) {
    if (lowered == alias) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::int64_t packet_timestamp_microseconds(const AVPacket& packet,
                                                         const AVRational& time_base) {
  std::int64_t timestamp = packet.pts;
  if (timestamp == AV_NOPTS_VALUE) {
    timestamp = packet.dts;
  }
  if (timestamp == AV_NOPTS_VALUE) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(timestamp, time_base, AVRational{1, 1'000'000});
}

[[nodiscard]] std::string text_from_ass_rect(const char* ass) {
  if (ass == nullptr) {
    return {};
  }
  const char* last_separator = ass;
  for (const char* cursor = ass; *cursor != '\0'; ++cursor) {
    if (cursor[0] == ',' && cursor[1] == ',') {
      last_separator = cursor + 2;
    }
  }
  return last_separator == ass ? std::string{} : std::string(last_separator);
}

[[nodiscard]] std::string cue_text_from_subtitle(const AVSubtitle& subtitle) {
  std::string text;
  for (unsigned index = 0; index < subtitle.num_rects; ++index) {
    const AVSubtitleRect* rect = subtitle.rects[index];
    if (rect == nullptr) {
      continue;
    }
    std::string rect_text;
    if (rect->type == SUBTITLE_TEXT && rect->text != nullptr) {
      rect_text = rect->text;
    } else if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
      rect_text = text_from_ass_rect(rect->ass);
    } else if (rect->type == SUBTITLE_BITMAP) {
      return {};
    }
    if (rect_text.empty()) {
      continue;
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += rect_text;
  }
  return text;
}

[[nodiscard]] std::int64_t packet_end_microseconds(const AVPacket& packet,
                                                     const AVRational& time_base) {
  if (packet.duration > 0) {
    return av_rescale_q(packet.pts + packet.duration, time_base, AVRational{1, 1'000'000});
  }
  return AV_NOPTS_VALUE;
}

} // namespace

bool is_text_subtitle_codec(const std::string_view codec_name) noexcept {
  return codec_alias_matches(codec_name, {"subrip", "srt", "mov_text", "tx3g", "text", "webvtt",
                                          "vtt"});
}

bool is_bitmap_subtitle_codec(const std::string_view codec_name) noexcept {
  return codec_alias_matches(codec_name,
                             {"hdmv_pgs_subtitle", "pgssub", "dvd_subtitle", "dvdsub", "dvb_subtitle",
                              "dvbsub", "xsub", "ass", "ssa", "sami", "jacosub", "microdvd",
                              "mpl2", "pjs", "realtext", "stl", "subviewer", "subviewer1"});
}

std::vector<SubtitleStreamInfo> list_subtitle_streams(const AssetDescriptor& asset) noexcept {
  std::vector<SubtitleStreamInfo> streams;
  for (const StreamDescriptor& stream : asset.streams) {
    if (stream.kind != StreamKind::Subtitle) {
      continue;
    }
    streams.push_back(SubtitleStreamInfo{
        .index = stream.index,
        .codec_name = stream.codec_name,
        .language = stream.language,
        .supported_text_codec = is_text_subtitle_codec(stream.codec_name),
    });
  }
  return streams;
}

Result<SubtitleExtraction> extract_text_subtitles(const std::filesystem::path& uri,
                                                  const int stream_index,
                                                  const SubtitleExtractOptions& options) {
  if (uri.empty()) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::InvalidArgument, 0, "media path is empty"));
  }
  if (stream_index < 0) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::InvalidArgument, 0, "subtitle stream index is invalid"));
  }
  if (!media_uri_exists(uri)) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::FileNotFound, 0, "media file does not exist"));
  }

  AVFormatContext* raw_context = avformat_alloc_context();
  if (raw_context == nullptr) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::Internal, AVERROR(ENOMEM), "cannot allocate format context"));
  }
  FormatContextPtr context(raw_context);
  context->interrupt_callback = {.callback = nullptr, .opaque = nullptr};
  apply_input_probe_options(*context, options.probe);

  AVFormatContext* opened_context = context.release();
  const int open_result = open_media_input(&opened_context, uri);
  context.reset(opened_context);
  if (open_result < 0) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::OpenFailed, open_result, "cannot open media"));
  }

  const int stream_result = inspect_input_streams(*context);
  if (stream_result < 0) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::ProbeFailed, stream_result, "cannot read stream information"));
  }

  if (stream_index >= static_cast<int>(context->nb_streams)) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::InvalidArgument, 0, "subtitle stream index is out of range"));
  }

  AVStream* stream = context->streams[stream_index];
  if (stream == nullptr || stream->codecpar == nullptr ||
      stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::InvalidArgument, 0, "stream is not a subtitle track"));
  }

  const AVCodecParameters& parameters = *stream->codecpar;
  const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters.codec_id);
  const std::string codec_name = descriptor == nullptr ? avcodec_get_name(parameters.codec_id)
                                                       : descriptor->name;
  if (!is_text_subtitle_codec(codec_name)) {
    const std::string message =
        is_bitmap_subtitle_codec(codec_name)
            ? "subtitle stream uses an unsupported bitmap or styled format: " + codec_name
            : "subtitle stream codec is not supported for text extraction: " + codec_name;
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::Unsupported, 0, message));
  }

  const AVCodec* codec = avcodec_find_decoder(parameters.codec_id);
  if (codec == nullptr) {
    return Result<SubtitleExtraction>::failure(make_error(
        MediaErrorCode::Unsupported, 0, "no decoder available for subtitle codec: " + codec_name));
  }

  AVCodecContext* raw_codec_context = avcodec_alloc_context3(codec);
  if (raw_codec_context == nullptr) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::Internal, AVERROR(ENOMEM), "cannot allocate subtitle decoder"));
  }
  CodecContextPtr codec_context(raw_codec_context);
  if (const int copy_result = avcodec_parameters_to_context(codec_context.get(), &parameters);
      copy_result < 0) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::Internal, copy_result, "cannot copy subtitle codec parameters"));
  }
  if (const int open_codec_result = avcodec_open2(codec_context.get(), codec, nullptr);
      open_codec_result < 0) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::OpenFailed, open_codec_result, "cannot open subtitle decoder"));
  }

  SubtitleExtraction extraction;
  extraction.stream_index = stream_index;
  extraction.codec_name = codec_name;
  const AVDictionaryEntry* language_entry =
      av_dict_get(stream->metadata, "language", nullptr, 0);
  if (language_entry != nullptr && language_entry->value != nullptr) {
    extraction.language = language_entry->value;
  }

  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) {
    return Result<SubtitleExtraction>::failure(
        make_error(MediaErrorCode::Internal, AVERROR(ENOMEM), "cannot allocate subtitle packet"));
  }

  while (av_read_frame(context.get(), packet) >= 0) {
    if (packet->stream_index != stream_index) {
      av_packet_unref(packet);
      continue;
    }

    AVSubtitle subtitle{};
    int got_subtitle = 0;
    const int decode_result =
        avcodec_decode_subtitle2(codec_context.get(), &subtitle, &got_subtitle, packet);
    if (decode_result < 0) {
      avsubtitle_free(&subtitle);
      av_packet_unref(packet);
      av_packet_free(&packet);
      return Result<SubtitleExtraction>::failure(
          make_error(MediaErrorCode::Internal, decode_result, "subtitle decode failed"));
    }
    if (got_subtitle == 0) {
      avsubtitle_free(&subtitle);
      av_packet_unref(packet);
      continue;
    }

    const std::string text = cue_text_from_subtitle(subtitle);
    const std::int64_t display_start_ms = subtitle.start_display_time;
    const std::int64_t display_end_ms = subtitle.end_display_time;
    avsubtitle_free(&subtitle);
    if (text.empty()) {
      av_packet_unref(packet);
      continue;
    }

    const std::int64_t packet_start =
        packet_timestamp_microseconds(*packet, stream->time_base);
    if (packet_start == AV_NOPTS_VALUE) {
      av_packet_unref(packet);
      continue;
    }

    std::int64_t start_microseconds = packet_start + display_start_ms * 1'000;
    std::int64_t end_microseconds = packet_start + display_end_ms * 1'000;
    if (display_end_ms <= display_start_ms) {
      start_microseconds = packet_start;
      end_microseconds = packet_end_microseconds(*packet, stream->time_base);
    }
    if (end_microseconds == AV_NOPTS_VALUE || end_microseconds <= start_microseconds) {
      av_packet_unref(packet);
      continue;
    }

    extraction.cues.push_back(ExtractedSubtitleCue{
        .start_microseconds = start_microseconds,
        .end_microseconds = end_microseconds,
        .text = text,
    });
    av_packet_unref(packet);
  }

  av_packet_free(&packet);

  std::ranges::sort(extraction.cues, [](const ExtractedSubtitleCue& left,
                                        const ExtractedSubtitleCue& right) {
    return left.start_microseconds < right.start_microseconds;
  });

  return Result<SubtitleExtraction>::success(std::move(extraction));
}

} // namespace video_editor::media

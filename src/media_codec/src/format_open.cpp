// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/format_open.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <array>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace video_editor::media {
namespace {

struct AttachmentTypeRestore {
  AVCodecParameters* parameters{nullptr};
  AVMediaType type{AVMEDIA_TYPE_UNKNOWN};
};

[[nodiscard]] bool path_looks_like_image_sequence_pattern(const std::filesystem::path& uri) {
  return uri.filename().string().find('%') != std::string::npos;
}

} // namespace

bool should_suppress_ffmpeg_log(const int level, const std::string_view message) noexcept {
  if ((level & 0xff) > AV_LOG_WARNING || message.empty()) {
    return false;
  }
  return message.find("Could not update timestamps for skipped samples") != std::string_view::npos ||
         message.find("deprecated pixel format used") != std::string_view::npos;
}

namespace {

void quiet_ffmpeg_log(void* pointer, const int level, const char* format, va_list arguments) {
  if (format != nullptr) {
    std::array<char, 512> buffer{};
    va_list copied;
    va_copy(copied, arguments);
    const int written = vsnprintf(buffer.data(), buffer.size(), format, copied);
    va_end(copied);
    if (written > 0 && should_suppress_ffmpeg_log(level, buffer.data())) {
      return;
    }
  }
  av_log_default_callback(pointer, level, format, arguments);
}

} // namespace

void install_quiet_ffmpeg_log_filter() {
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_callback(quiet_ffmpeg_log); });
}

void apply_input_probe_options(AVFormatContext& context, const ProbeOptions& options) {
  context.probesize = options.probe_size_bytes;
  context.max_analyze_duration = options.analyze_duration_microseconds;
}

void discard_undecodable_input_streams(AVFormatContext& context) {
  for (unsigned index = 0; index < context.nb_streams; ++index) {
    AVStream* stream = context.streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) {
      continue;
    }
    const AVMediaType type = stream->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_ATTACHMENT || type == AVMEDIA_TYPE_DATA) {
      stream->discard = AVDISCARD_ALL;
    }
  }
}

bool media_uri_exists(const std::filesystem::path& uri) {
  std::error_code error;
  if (std::filesystem::is_regular_file(uri, error)) {
    return true;
  }
  if (!path_looks_like_image_sequence_pattern(uri)) {
    return false;
  }
  const std::string pattern = uri.filename().string();
  const auto parent = uri.parent_path();
  for (int index = 0; index < 10'000; ++index) {
    std::array<char, 512> name{};
    if (std::snprintf(name.data(), name.size(), pattern.c_str(), index) <= 0) {
      return false;
    }
    if (std::filesystem::is_regular_file(parent / name.data(), error)) {
      return true;
    }
  }
  return false;
}

int open_media_input(AVFormatContext** context, const std::filesystem::path& uri) {
  const std::string path = uri.string();
  const AVInputFormat* format = nullptr;
  AVDictionary* options = nullptr;
  if (path_looks_like_image_sequence_pattern(uri)) {
    format = av_find_input_format("image2");
    av_dict_set(&options, "framerate", "30", 0);
    const std::string pattern = uri.filename().string();
    const auto parent = uri.parent_path();
    std::error_code error;
    for (int index = 0; index < 10'000; ++index) {
      std::array<char, 512> name{};
      if (std::snprintf(name.data(), name.size(), pattern.c_str(), index) <= 0) {
        break;
      }
      if (std::filesystem::is_regular_file(parent / name.data(), error)) {
        av_dict_set_int(&options, "start_number", index, 0);
        break;
      }
    }
  }
  const int result = avformat_open_input(context, path.c_str(), format, &options);
  av_dict_free(&options);
  return result;
}

int inspect_input_streams(AVFormatContext& context) {
  discard_undecodable_input_streams(context);

  // FFmpeg 8+/9 still warns "unknown codec" for ATTACHMENT + NONE at the end of
  // avformat_find_stream_info, even when the stream is discarded. DATA + NONE is
  // treated as complete; restore the original type afterward so probe metadata
  // still reports StreamKind::Attachment.
  std::vector<AttachmentTypeRestore> restore;
  restore.reserve(context.nb_streams);
  for (unsigned index = 0; index < context.nb_streams; ++index) {
    AVStream* stream = context.streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) {
      continue;
    }
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT &&
        stream->codecpar->codec_id == AV_CODEC_ID_NONE) {
      restore.push_back({.parameters = stream->codecpar, .type = AVMEDIA_TYPE_ATTACHMENT});
      stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    }
  }

  const int status = avformat_find_stream_info(&context, nullptr);
  for (const AttachmentTypeRestore& entry : restore) {
    if (entry.parameters != nullptr) {
      entry.parameters->codec_type = entry.type;
    }
  }
  return status;
}

} // namespace video_editor::media

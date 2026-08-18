// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/format_open.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <array>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <vector>

namespace video_editor::media {
namespace {

struct AttachmentTypeRestore {
  AVCodecParameters* parameters{nullptr};
  AVMediaType type{AVMEDIA_TYPE_UNKNOWN};
};

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

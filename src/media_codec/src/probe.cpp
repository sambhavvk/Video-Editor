// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/format_open.h"
#include "video_editor/media_codec/probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <system_error>

namespace video_editor::media {
namespace {

struct FormatContextCloser {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      avformat_close_input(&context);
    }
  }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextCloser>;

std::string ffmpeg_error_string(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return buffer.data();
}

MediaError make_error(const MediaErrorCode code, const int native_code,
                      std::string message) {
  if (native_code < 0) {
    message += ": " + ffmpeg_error_string(native_code);
  }
  return {.code = code, .native_code = native_code, .message = std::move(message)};
}

Rational rational(const AVRational value) {
  return {.numerator = value.num, .denominator = value.den == 0 ? 1 : value.den};
}

std::string dictionary_value(const AVDictionary* dictionary, const char* key) {
  const AVDictionaryEntry* entry = av_dict_get(dictionary, key, nullptr, 0);
  return entry == nullptr ? std::string{} : std::string(entry->value);
}

std::map<std::string, std::string> dictionary(const AVDictionary* source) {
  std::map<std::string, std::string> result;
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_iterate(source, entry)) != nullptr) {
    result.emplace(entry->key, entry->value);
  }
  return result;
}

StreamKind stream_kind(const AVMediaType type) {
  switch (type) {
  case AVMEDIA_TYPE_VIDEO:
    return StreamKind::Video;
  case AVMEDIA_TYPE_AUDIO:
    return StreamKind::Audio;
  case AVMEDIA_TYPE_SUBTITLE:
    return StreamKind::Subtitle;
  case AVMEDIA_TYPE_DATA:
    return StreamKind::Data;
  case AVMEDIA_TYPE_ATTACHMENT:
    return StreamKind::Attachment;
  default:
    return StreamKind::Unknown;
  }
}

bool rate_differs(const AVRational left, const AVRational right) {
  if (left.num == 0 || left.den == 0 || right.num == 0 || right.den == 0) {
    return false;
  }
  return av_cmp_q(left, right) != 0;
}

const char* safe_name(const char* value) { return value == nullptr ? "unknown" : value; }

std::string color_primaries_name(const AVColorPrimaries value) {
  return safe_name(av_color_primaries_name(value));
}

std::string color_transfer_name(const AVColorTransferCharacteristic value) {
  return safe_name(av_color_transfer_name(value));
}

std::string color_space_name(const AVColorSpace value) {
  return safe_name(av_color_space_name(value));
}

std::string color_range_name(const AVColorRange value) {
  return safe_name(av_color_range_name(value));
}

std::string chroma_location_name(const AVChromaLocation value) {
  return safe_name(av_chroma_location_name(value));
}

std::string field_order_name(const AVFieldOrder value) {
  switch (value) {
  case AV_FIELD_PROGRESSIVE:
    return "progressive";
  case AV_FIELD_TT:
    return "top-coded-top-displayed";
  case AV_FIELD_BB:
    return "bottom-coded-bottom-displayed";
  case AV_FIELD_TB:
    return "top-coded-bottom-displayed";
  case AV_FIELD_BT:
    return "bottom-coded-top-displayed";
  case AV_FIELD_UNKNOWN:
  default:
    return "unknown";
  }
}

VideoDescription video_description(const AVStream& stream) {
  const AVCodecParameters& parameters = *stream.codecpar;
  VideoDescription video;
  video.width = parameters.width;
  video.height = parameters.height;
  video.sample_aspect_ratio = rational(parameters.sample_aspect_ratio);
  video.average_frame_rate = rational(stream.avg_frame_rate);
  video.real_frame_rate = rational(stream.r_frame_rate);
  video.frame_count = stream.nb_frames;
  video.variable_frame_rate_evidence = rate_differs(stream.avg_frame_rate, stream.r_frame_rate);
  video.field_order = field_order_name(parameters.field_order);
  video.color = {
      .primaries = color_primaries_name(parameters.color_primaries),
      .transfer = color_transfer_name(parameters.color_trc),
      .matrix = color_space_name(parameters.color_space),
      .range = color_range_name(parameters.color_range),
      .chroma_location = chroma_location_name(parameters.chroma_location),
  };

  const auto pixel_format = static_cast<AVPixelFormat>(parameters.format);
  video.pixel_format = safe_name(av_get_pix_fmt_name(pixel_format));
  if (const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixel_format);
      descriptor != nullptr) {
    video.bit_depth = descriptor->comp[0].depth;
    video.has_alpha = (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
  } else {
    video.bit_depth = parameters.bits_per_raw_sample;
  }

  const std::string rotation = dictionary_value(stream.metadata, "rotate");
  if (!rotation.empty()) {
    try {
      video.rotation_degrees = std::stoi(rotation);
    } catch (const std::exception&) {
      video.rotation_degrees = 0;
    }
  }
  return video;
}

AudioDescription audio_description(const AVCodecParameters& parameters) {
  AudioDescription audio;
  audio.sample_rate = parameters.sample_rate;
  audio.channels = parameters.ch_layout.nb_channels;
  audio.bits_per_sample = parameters.bits_per_raw_sample;
  audio.sample_format = safe_name(av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters.format)));

  std::array<char, 256> channel_layout{};
  if (av_channel_layout_describe(&parameters.ch_layout, channel_layout.data(),
                                 channel_layout.size()) >= 0) {
    audio.channel_layout = channel_layout.data();
  }
  return audio;
}

int interrupt_callback(void* opaque) {
  const auto* cancelled = static_cast<const std::atomic_bool*>(opaque);
  return cancelled != nullptr && cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace

Result<AssetDescriptor> probe(const std::filesystem::path& uri, const ProbeOptions& options) {
  if (uri.empty()) {
    return Result<AssetDescriptor>::failure(
        make_error(MediaErrorCode::InvalidArgument, 0, "media path is empty"));
  }

  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(uri, filesystem_error)) {
    return Result<AssetDescriptor>::failure(
        make_error(MediaErrorCode::FileNotFound, 0, "media file does not exist"));
  }

  AVFormatContext* raw_context = avformat_alloc_context();
  if (raw_context == nullptr) {
    return Result<AssetDescriptor>::failure(
        make_error(MediaErrorCode::Internal, AVERROR(ENOMEM), "cannot allocate format context"));
  }
  FormatContextPtr context(raw_context);
  context->interrupt_callback = {.callback = interrupt_callback,
                                 .opaque = const_cast<std::atomic_bool*>(options.cancel)};
  apply_input_probe_options(*context, options);

  AVFormatContext* opened_context = context.release();
  const std::string path = uri.string();
  const int open_result = avformat_open_input(&opened_context, path.c_str(), nullptr, nullptr);
  context.reset(opened_context);
  if (open_result < 0) {
    const bool cancelled = options.cancel != nullptr && options.cancel->load();
    return Result<AssetDescriptor>::failure(make_error(
        cancelled ? MediaErrorCode::Cancelled : MediaErrorCode::OpenFailed, open_result,
        cancelled ? "media probe cancelled" : "cannot open media"));
  }

  const int stream_result = inspect_input_streams(*context);
  if (stream_result < 0) {
    const bool cancelled = options.cancel != nullptr && options.cancel->load();
    return Result<AssetDescriptor>::failure(make_error(
        cancelled ? MediaErrorCode::Cancelled : MediaErrorCode::ProbeFailed, stream_result,
        cancelled ? "media probe cancelled" : "cannot read stream information"));
  }

  AssetDescriptor asset;
  asset.uri = std::filesystem::absolute(uri);
  if (context->iformat != nullptr) {
    asset.format_name = safe_name(context->iformat->name);
    asset.format_long_name = safe_name(context->iformat->long_name);
  }
  if (context->start_time != AV_NOPTS_VALUE) {
    asset.start_time_microseconds = context->start_time;
  }
  if (context->duration != AV_NOPTS_VALUE) {
    asset.duration_microseconds = context->duration;
  }
  asset.bit_rate = context->bit_rate;
  asset.metadata = dictionary(context->metadata);
  asset.best_video_stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                                nullptr, 0);
  if (asset.best_video_stream < 0) {
    asset.best_video_stream = -1;
  }
  asset.best_audio_stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_AUDIO, -1, -1,
                                                nullptr, 0);
  if (asset.best_audio_stream < 0) {
    asset.best_audio_stream = -1;
  }

  asset.streams.reserve(context->nb_streams);
  for (unsigned index = 0; index < context->nb_streams; ++index) {
    if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
      return Result<AssetDescriptor>::failure(
          make_error(MediaErrorCode::Cancelled, AVERROR_EXIT, "media probe cancelled"));
    }

    const AVStream& stream = *context->streams[index];
    const AVCodecParameters& parameters = *stream.codecpar;
    const AVCodecDescriptor* codec = avcodec_descriptor_get(parameters.codec_id);
    StreamDescriptor descriptor;
    descriptor.index = static_cast<int>(index);
    descriptor.kind = stream_kind(parameters.codec_type);
    descriptor.codec_name = codec == nullptr ? avcodec_get_name(parameters.codec_id)
                                             : safe_name(codec->name);
    descriptor.codec_long_name = codec == nullptr ? descriptor.codec_name : safe_name(codec->long_name);
    descriptor.time_base = rational(stream.time_base);
    descriptor.bit_rate = parameters.bit_rate;
    descriptor.disposition = stream.disposition;
    descriptor.attached_picture = (stream.disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    descriptor.language = dictionary_value(stream.metadata, "language");
    descriptor.metadata = dictionary(stream.metadata);
    if (stream.start_time != AV_NOPTS_VALUE) {
      descriptor.start_time = stream.start_time;
    }
    if (stream.duration != AV_NOPTS_VALUE) {
      descriptor.duration = stream.duration;
    }
    if (parameters.codec_type == AVMEDIA_TYPE_VIDEO) {
      descriptor.video = video_description(stream);
    } else if (parameters.codec_type == AVMEDIA_TYPE_AUDIO) {
      descriptor.audio = audio_description(parameters);
    }
    asset.streams.push_back(std::move(descriptor));
  }

  return Result<AssetDescriptor>::success(std::move(asset));
}

} // namespace video_editor::media

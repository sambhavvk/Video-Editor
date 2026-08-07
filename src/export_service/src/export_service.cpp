// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/export_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace video_editor::export_service {
namespace {

constexpr int kMaximumDimension = 32'768;
constexpr std::uint64_t kMaximumFrameCount =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

struct FormatContextCloser final {
  void operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) {
      return;
    }
    if (context->pb != nullptr) {
      avio_closep(&context->pb);
    }
    avformat_free_context(context);
  }
};

struct CodecContextCloser final {
  void operator()(AVCodecContext* context) const noexcept {
    avcodec_free_context(&context);
  }
};

struct FrameCloser final {
  void operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
  }
};

struct PacketCloser final {
  void operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
  }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextCloser>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;

class TemporaryFile final {
public:
  explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
  ~TemporaryFile() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }
  void release() noexcept {
    committed_ = true;
  }

private:
  std::filesystem::path path_;
  bool committed_{false};
};

struct PresetConfiguration final {
  const char* muxer_name;
  const char* container_name;
  const char* codec_name;
  AVCodecID codec_id;
  AVPixelFormat pixel_format;
  bool lossless;
};

[[nodiscard]] PresetConfiguration configuration_for(const VideoPreset preset) {
  switch (preset) {
  case VideoPreset::Ffv1Matroska:
    return {.muxer_name = "matroska",
            .container_name = "Matroska",
            .codec_name = "FFV1",
            .codec_id = AV_CODEC_ID_FFV1,
            .pixel_format = AV_PIX_FMT_YUV444P10LE,
            .lossless = true};
  case VideoPreset::ProRes422HqMov:
    return {.muxer_name = "mov",
            .container_name = "QuickTime / MOV",
            .codec_name = "Apple ProRes 422 HQ",
            .codec_id = AV_CODEC_ID_PRORES,
            .pixel_format = AV_PIX_FMT_YUV422P10LE,
            .lossless = false};
  }
  throw std::invalid_argument("unknown export preset");
}

[[nodiscard]] const AVCodec* encoder_for(const VideoPreset preset) noexcept {
  if (preset == VideoPreset::ProRes422HqMov) {
    if (const AVCodec* encoder = avcodec_find_encoder_by_name("prores_ks"); encoder != nullptr) {
      return encoder;
    }
  }
  const AVCodecID codec_id =
      preset == VideoPreset::Ffv1Matroska ? AV_CODEC_ID_FFV1 : AV_CODEC_ID_PRORES;
  return avcodec_find_encoder(codec_id);
}

[[nodiscard]] bool encoder_supports_pixel_format(const AVCodec& encoder,
                                                 const AVPixelFormat pixel_format) noexcept {
  const void* raw_configurations = nullptr;
  int configuration_count = 0;
  const int status = avcodec_get_supported_config(nullptr, &encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                                  &raw_configurations, &configuration_count);
  if (status < 0) {
    return false;
  }
  if (raw_configurations == nullptr) {
    return true;
  }
  const auto* formats = static_cast<const AVPixelFormat*>(raw_configurations);
  return std::find(formats, formats + configuration_count, pixel_format) !=
         formats + configuration_count;
}

[[nodiscard]] std::string path_string(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] std::string ffmpeg_error(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return buffer.data();
}

[[nodiscard]] ExportOutcome failure(const ExportErrorCode code, std::string message) {
  return ExportOutcome::failure({.code = code, .message = std::move(message)});
}

[[nodiscard]] std::filesystem::path make_temporary_path(const std::filesystem::path& destination) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto nonce =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
  const auto parent =
      destination.has_parent_path() ? destination.parent_path() : std::filesystem::path{"."};
  const std::string base = destination.filename().string();
  for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
    auto candidate = parent / ("." + base + ".export-" + std::to_string(nonce) + "-" +
                               std::to_string(id) + "-" + std::to_string(attempt) + ".tmp");
    std::error_code error;
    if (!std::filesystem::exists(candidate, error) && !error) {
      return candidate;
    }
  }
  throw std::runtime_error("could not allocate a unique temporary export path");
}

[[nodiscard]] bool sync_file(const std::filesystem::path& path, std::string& message) {
#ifdef _WIN32
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    message = "could not open temporary export for flushing";
    return false;
  }
  const bool flushed = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
  if (!flushed) {
    message = "could not flush temporary export to disk";
  }
  return flushed;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    message = "could not open temporary export for flushing: " +
              std::error_code(errno, std::generic_category()).message();
    return false;
  }
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    message = "could not flush temporary export to disk: " +
              std::error_code(saved_errno, std::generic_category()).message();
    return false;
  }
  return true;
#endif
}

[[nodiscard]] std::error_code atomic_commit(const std::filesystem::path& temporary,
                                            const std::filesystem::path& destination,
                                            const bool overwrite) noexcept {
#ifdef _WIN32
  const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U);
  if (MoveFileExW(temporary.c_str(), destination.c_str(), flags) != 0) {
    return {};
  }
  return std::error_code(static_cast<int>(GetLastError()), std::system_category());
#elif defined(__linux__)
  if (overwrite) {
    if (::rename(temporary.c_str(), destination.c_str()) == 0) {
      return {};
    }
    return std::error_code(errno, std::generic_category());
  }
  const long result = ::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                                destination.c_str(), RENAME_NOREPLACE);
  if (result == 0) {
    return {};
  }
  return std::error_code(errno, std::generic_category());
#else
  std::error_code exists_error;
  if (!overwrite && std::filesystem::exists(destination, exists_error)) {
    return std::make_error_code(std::errc::file_exists);
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, destination, rename_error);
  return rename_error;
#endif
}

[[nodiscard]] double rec709_oetf(const float linear_value) noexcept {
  const double value = std::clamp(static_cast<double>(linear_value), 0.0, 1.0);
  if (value < 0.018) {
    return 4.5 * value;
  }
  return (1.099 * std::pow(value, 0.45)) - 0.099;
}

struct YuvSample final {
  double y;
  double cb;
  double cr;
};

[[nodiscard]] YuvSample to_rec709_yuv(const std::span<const float, 4> pixel) noexcept {
  const double red = rec709_oetf(pixel[0]);
  const double green = rec709_oetf(pixel[1]);
  const double blue = rec709_oetf(pixel[2]);
  const double luminance = (0.2126 * red) + (0.7152 * green) + (0.0722 * blue);
  return {.y = luminance, .cb = (blue - luminance) / 1.8556, .cr = (red - luminance) / 1.5748};
}

[[nodiscard]] std::uint16_t quantize_luma(const double value) noexcept {
  const double scaled = 64.0 + (876.0 * std::clamp(value, 0.0, 1.0));
  return static_cast<std::uint16_t>(std::floor(scaled + 0.5));
}

[[nodiscard]] std::uint16_t quantize_chroma(const double value) noexcept {
  const double scaled = 512.0 + (896.0 * std::clamp(value, -0.5, 0.5));
  return static_cast<std::uint16_t>(std::floor(scaled + 0.5));
}

void write_16_bit_sample(std::uint8_t* destination, const std::uint16_t value) noexcept {
  AV_WL16(destination, value);
}

[[nodiscard]] bool convert_frame(const render::VideoFrame& source, AVFrame& destination,
                                 std::string& message) {
  if (source.layout != render::PixelLayout::RgbaFloat32 ||
      !std::holds_alternative<std::shared_ptr<const render::CpuFrame>>(source.storage)) {
    message = "renderer did not return a CPU linear RGBA frame";
    return false;
  }
  const auto& cpu = std::get<std::shared_ptr<const render::CpuFrame>>(source.storage);
  if (!cpu || cpu->width() != destination.width || cpu->height() != destination.height) {
    message = "renderer returned a frame with unexpected dimensions";
    return false;
  }
  const auto writable = av_frame_make_writable(&destination);
  if (writable < 0) {
    message = "could not make encoder frame writable: " + ffmpeg_error(writable);
    return false;
  }

  const bool subsampled = destination.format == AV_PIX_FMT_YUV422P10LE;
  for (int y = 0; y < destination.height; ++y) {
    auto* luma_row =
        destination.data[0] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[0]);
    for (int x = 0; x < destination.width; ++x) {
      const auto sample = to_rec709_yuv(cpu->pixel(x, y));
      write_16_bit_sample(luma_row + (static_cast<std::ptrdiff_t>(x) * 2), quantize_luma(sample.y));
    }

    auto* cb_row = destination.data[1] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[1]);
    auto* cr_row = destination.data[2] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[2]);
    if (subsampled) {
      for (int x = 0; x < destination.width; x += 2) {
        const auto left = to_rec709_yuv(cpu->pixel(x, y));
        const auto right = to_rec709_yuv(cpu->pixel(x + 1, y));
        const auto index = static_cast<std::ptrdiff_t>(x / 2) * 2;
        write_16_bit_sample(cb_row + index, quantize_chroma((left.cb + right.cb) * 0.5));
        write_16_bit_sample(cr_row + index, quantize_chroma((left.cr + right.cr) * 0.5));
      }
    } else {
      for (int x = 0; x < destination.width; ++x) {
        const auto sample = to_rec709_yuv(cpu->pixel(x, y));
        const auto index = static_cast<std::ptrdiff_t>(x) * 2;
        write_16_bit_sample(cb_row + index, quantize_chroma(sample.cb));
        write_16_bit_sample(cr_row + index, quantize_chroma(sample.cr));
      }
    }
  }
  return true;
}

[[nodiscard]] int drain_packets(AVCodecContext& codec, AVFormatContext& format, AVStream& stream,
                                AVPacket& packet) {
  while (true) {
    const int received = avcodec_receive_packet(&codec, &packet);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
      return 0;
    }
    if (received < 0) {
      return received;
    }
    av_packet_rescale_ts(&packet, codec.time_base, stream.time_base);
    packet.stream_index = stream.index;
    const int written = av_interleaved_write_frame(&format, &packet);
    av_packet_unref(&packet);
    if (written < 0) {
      return written;
    }
  }
}

[[nodiscard]] std::optional<ExportError>
report_progress(const ExportRequest& request, const std::uint64_t completed,
                const std::uint64_t total, const edit::Time duration, const edit::Rate& rate) {
  if (!request.progress) {
    return std::nullopt;
  }
  try {
    const edit::Time completed_time = rate.frameTime(static_cast<std::int64_t>(completed));
    request.progress({.completed_frames = completed,
                      .total_frames = total,
                      .timeline_time = std::min(completed_time, duration),
                      .fraction = static_cast<double>(completed) / static_cast<double>(total)});
  } catch (const std::exception& exception) {
    return ExportError{.code = ExportErrorCode::ProgressCallbackFailed,
                       .message =
                           std::string{"export progress callback failed: "} + exception.what()};
  } catch (...) {
    return ExportError{.code = ExportErrorCode::ProgressCallbackFailed,
                       .message = "export progress callback failed"};
  }
  return std::nullopt;
}

} // namespace

PresetInfo preset_info(const VideoPreset preset) {
  const auto configuration = configuration_for(preset);
  return {.preset = preset,
          .display_name = preset == VideoPreset::Ffv1Matroska ? "FFV1 10-bit / Matroska (lossless)"
                                                              : "Apple ProRes 422 HQ / MOV",
          .container = configuration.container_name,
          .codec = configuration.codec_name,
          .available =
              [preset, &configuration] {
                const AVCodec* encoder = encoder_for(preset);
                return encoder != nullptr &&
                       encoder_supports_pixel_format(*encoder, configuration.pixel_format);
              }(),
          .lossless = configuration.lossless};
}

ExportOutcome export_video(const ExportRequest& request) {
  try {
    if (request.renderer == nullptr) {
      return failure(ExportErrorCode::InvalidRequest, "export requires a CPU renderer");
    }
    if (request.destination.empty() || request.destination.filename().empty()) {
      return failure(ExportErrorCode::InvalidRequest, "export destination must name a file");
    }
    if (request.include_audio) {
      return failure(ExportErrorCode::AudioNotSupported,
                     "this beta export service is video-only; audio export was requested");
    }
    if (request.preset != VideoPreset::Ffv1Matroska &&
        request.preset != VideoPreset::ProRes422HqMov) {
      return failure(ExportErrorCode::InvalidRequest, "unknown export preset");
    }
    if (request.cancellation.stop_requested()) {
      return failure(ExportErrorCode::Cancelled, "export was cancelled before it started");
    }

    const edit::Sequence* sequence = nullptr;
    try {
      sequence = &request.snapshot.sequence();
    } catch (const std::exception& exception) {
      return failure(ExportErrorCode::InvalidRequest,
                     std::string{"export snapshot is invalid: "} + exception.what());
    }
    if (sequence->width == 0 || sequence->height == 0 || sequence->width > kMaximumDimension ||
        sequence->height > kMaximumDimension) {
      return failure(ExportErrorCode::InvalidRequest,
                     "sequence dimensions are outside the supported export range");
    }
    if (sequence->frame_rate.numerator() >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        sequence->frame_rate.denominator() >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      return failure(ExportErrorCode::InvalidRequest,
                     "sequence frame rate cannot be represented by FFmpeg");
    }

    const auto configuration = configuration_for(request.preset);
    if (configuration.pixel_format == AV_PIX_FMT_YUV422P10LE && (sequence->width % 2U) != 0U) {
      return failure(ExportErrorCode::InvalidRequest,
                     "ProRes 4:2:2 export requires an even sequence width");
    }
    const AVCodec* encoder = encoder_for(request.preset);
    if (encoder == nullptr ||
        !encoder_supports_pixel_format(*encoder, configuration.pixel_format)) {
      return failure(ExportErrorCode::EncoderUnavailable,
                     std::string{configuration.codec_name} + " encoder is not available");
    }

    const edit::Time duration = request.snapshot.duration();
    if (duration.isZero() || duration.isNegative()) {
      return failure(ExportErrorCode::InvalidRequest, "cannot export an empty sequence");
    }
    const std::int64_t signed_frame_count =
        sequence->frame_rate.framesAt(duration, edit::RoundingMode::Ceil);
    if (signed_frame_count <= 0 ||
        static_cast<std::uint64_t>(signed_frame_count) > kMaximumFrameCount) {
      return failure(ExportErrorCode::InvalidRequest, "sequence frame count is not exportable");
    }
    const auto frame_count = static_cast<std::uint64_t>(signed_frame_count);

    std::error_code filesystem_error;
    const auto parent = request.destination.has_parent_path() ? request.destination.parent_path()
                                                              : std::filesystem::path{"."};
    if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
      return failure(ExportErrorCode::InvalidRequest,
                     "export destination parent is not an accessible directory");
    }
    filesystem_error.clear();
    const bool destination_exists = std::filesystem::exists(request.destination, filesystem_error);
    if (filesystem_error) {
      return failure(ExportErrorCode::IoFailed,
                     "could not inspect export destination: " + filesystem_error.message());
    }
    if (destination_exists && !request.overwrite_existing) {
      return failure(ExportErrorCode::DestinationExists,
                     "export destination already exists and overwrite is disabled");
    }
    filesystem_error.clear();
    const bool destination_is_directory =
        destination_exists && std::filesystem::is_directory(request.destination, filesystem_error);
    if (filesystem_error) {
      return failure(ExportErrorCode::IoFailed,
                     "could not inspect export destination type: " + filesystem_error.message());
    }
    if (destination_is_directory) {
      return failure(ExportErrorCode::InvalidRequest, "export destination is a directory");
    }

    TemporaryFile temporary(make_temporary_path(request.destination));
    const std::string temporary_name = path_string(temporary.path());
    AVFormatContext* raw_format = nullptr;
    int status = avformat_alloc_output_context2(&raw_format, nullptr, configuration.muxer_name,
                                                temporary_name.c_str());
    if (status < 0 || raw_format == nullptr) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not create export container: " + ffmpeg_error(status));
    }
    FormatContextPtr format(raw_format);
    format->flags |= AVFMT_FLAG_BITEXACT;

    AVStream* stream = avformat_new_stream(format.get(), nullptr);
    if (stream == nullptr) {
      return failure(ExportErrorCode::EncodingFailed, "could not create export video stream");
    }

    CodecContextPtr codec(avcodec_alloc_context3(encoder));
    if (!codec) {
      return failure(ExportErrorCode::EncodingFailed, "could not allocate video encoder");
    }
    codec->codec_id = configuration.codec_id;
    codec->codec_type = AVMEDIA_TYPE_VIDEO;
    codec->width = static_cast<int>(sequence->width);
    codec->height = static_cast<int>(sequence->height);
    codec->pix_fmt = configuration.pixel_format;
    codec->time_base = {static_cast<int>(sequence->frame_rate.denominator()),
                        static_cast<int>(sequence->frame_rate.numerator())};
    codec->framerate = {static_cast<int>(sequence->frame_rate.numerator()),
                        static_cast<int>(sequence->frame_rate.denominator())};
    codec->gop_size = 1;
    codec->thread_count = 1;
    codec->flags |= AV_CODEC_FLAG_BITEXACT;
    codec->color_primaries = AVCOL_PRI_BT709;
    codec->color_trc = AVCOL_TRC_BT709;
    codec->colorspace = AVCOL_SPC_BT709;
    codec->color_range = AVCOL_RANGE_MPEG;
    codec->chroma_sample_location = configuration.pixel_format == AV_PIX_FMT_YUV422P10LE
                                        ? AVCHROMA_LOC_LEFT
                                        : AVCHROMA_LOC_UNSPECIFIED;
    if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (request.preset == VideoPreset::ProRes422HqMov) {
      codec->profile = AV_PROFILE_PRORES_HQ;
      codec->codec_tag = MKTAG('a', 'p', 'c', 'h');
      av_opt_set(codec->priv_data, "vendor", "apl0", 0);
    } else {
      av_opt_set_int(codec->priv_data, "level", 3, 0);
      av_opt_set_int(codec->priv_data, "slicecrc", 1, 0);
    }

    status = avcodec_open2(codec.get(), encoder, nullptr);
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed, std::string{"could not open "} +
                                                          configuration.codec_name +
                                                          " encoder: " + ffmpeg_error(status));
    }
    status = avcodec_parameters_from_context(stream->codecpar, codec.get());
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not configure export stream: " + ffmpeg_error(status));
    }
    stream->time_base = codec->time_base;
    stream->avg_frame_rate = codec->framerate;
    stream->r_frame_rate = codec->framerate;

    status = avio_open(&format->pb, temporary_name.c_str(), AVIO_FLAG_WRITE);
    if (status < 0) {
      return failure(ExportErrorCode::IoFailed,
                     "could not open temporary export: " + ffmpeg_error(status));
    }
    AVDictionary* header_options = nullptr;
    av_dict_set(&header_options, "fflags", "+bitexact", 0);
    status = avformat_write_header(format.get(), &header_options);
    av_dict_free(&header_options);
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not write export header: " + ffmpeg_error(status));
    }

    FramePtr frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if (!frame || !packet) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not allocate FFmpeg export frame buffers");
    }
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->chroma_location = codec->chroma_sample_location;
    status = av_frame_get_buffer(frame.get(), 32);
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not allocate encoder pixels: " + ffmpeg_error(status));
    }

    static std::atomic<std::uint64_t> next_epoch{1};
    const std::uint64_t epoch = next_epoch.fetch_add(1, std::memory_order_relaxed);
    request.renderer->begin_epoch(epoch);
    constexpr render::PreviewProfile full_quality{.scale = render::PreviewScale::Full,
                                                  .bypass_expensive_effects = false,
                                                  .use_proxies = false};

    for (std::uint64_t index = 0; index < frame_count; ++index) {
      if (request.cancellation.stop_requested()) {
        return failure(ExportErrorCode::Cancelled, "export was cancelled");
      }
      const edit::Time timeline_time =
          sequence->frame_rate.frameTime(static_cast<std::int64_t>(index));
      auto rendered =
          request.renderer->request_frame(request.snapshot, timeline_time, full_quality, epoch);
      if (!rendered) {
        return failure(ExportErrorCode::RenderFailed, "could not render export frame " +
                                                          std::to_string(index) + ": " +
                                                          rendered.error->message);
      }
      std::string conversion_error;
      if (!convert_frame(*rendered.value, *frame, conversion_error)) {
        return failure(ExportErrorCode::RenderFailed, std::move(conversion_error));
      }
      frame->pts = static_cast<std::int64_t>(index);
      frame->duration = 1;
      status = avcodec_send_frame(codec.get(), frame.get());
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not submit export frame: " + ffmpeg_error(status));
      }
      status = drain_packets(*codec, *format, *stream, *packet);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not encode export frame: " + ffmpeg_error(status));
      }
      if (auto callback_error =
              report_progress(request, index + 1U, frame_count, duration, sequence->frame_rate)) {
        return ExportOutcome::failure(std::move(*callback_error));
      }
      if (request.cancellation.stop_requested()) {
        return failure(ExportErrorCode::Cancelled, "export was cancelled");
      }
    }

    status = avcodec_send_frame(codec.get(), nullptr);
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not flush video encoder: " + ffmpeg_error(status));
    }
    status = drain_packets(*codec, *format, *stream, *packet);
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not finish video encoding: " + ffmpeg_error(status));
    }
    if (request.cancellation.stop_requested()) {
      return failure(ExportErrorCode::Cancelled, "export was cancelled");
    }
    status = av_write_trailer(format.get());
    if (status < 0) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not finalize export container: " + ffmpeg_error(status));
    }
    status = avio_closep(&format->pb);
    if (status < 0) {
      return failure(ExportErrorCode::IoFailed,
                     "could not close temporary export: " + ffmpeg_error(status));
    }
    if (request.cancellation.stop_requested()) {
      return failure(ExportErrorCode::Cancelled, "export was cancelled");
    }

    std::string sync_error;
    if (!sync_file(temporary.path(), sync_error)) {
      return failure(ExportErrorCode::IoFailed, std::move(sync_error));
    }
    if (request.cancellation.stop_requested()) {
      return failure(ExportErrorCode::Cancelled, "export was cancelled");
    }
    ExportResult completed_export{
        .destination = request.destination,
        .preset = request.preset,
        .container = configuration.container_name,
        .video_codec = configuration.codec_name,
        .frame_count = frame_count,
        .source_timeline_duration = duration,
        .encoded_video_duration = sequence->frame_rate.frameTime(signed_frame_count),
        .video_exported = true,
        .audio_exported = false,
    };
    const std::error_code commit_error =
        atomic_commit(temporary.path(), request.destination, request.overwrite_existing);
    if (commit_error) {
      if (commit_error == std::errc::file_exists) {
        return failure(ExportErrorCode::DestinationExists,
                       "export destination appeared before the atomic commit");
      }
      return failure(ExportErrorCode::CommitFailed,
                     "could not atomically commit export: " + commit_error.message());
    }
    temporary.release();
    return ExportOutcome::success(std::move(completed_export));
  } catch (const std::exception& exception) {
    return failure(ExportErrorCode::EncodingFailed,
                   std::string{"unexpected export failure: "} + exception.what());
  } catch (...) {
    return failure(ExportErrorCode::EncodingFailed, "unexpected export failure");
  }
}

} // namespace video_editor::export_service

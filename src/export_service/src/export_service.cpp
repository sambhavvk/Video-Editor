// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/export_service.h"
#include "export_service_testing.hpp"
#include "video_editor/export_service/caption_burn_in.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
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

std::atomic<int> g_hardware_failure_injection{0};

constexpr int kMaximumDimension = 32'768;
constexpr std::uint64_t kMaximumFrameCount =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
// Twenty milliseconds is small enough for bounded interleave and aligns every
// packet start with Matroska's 1 ms timestamp scale at 48 kHz.
constexpr std::size_t kAudioBlockSamples = 960;
constexpr AVCodecID kAudioCodecId = AV_CODEC_ID_PCM_S16LE;
constexpr AVSampleFormat kAudioSampleFormat = AV_SAMPLE_FMT_S16;
constexpr const char* kAudioCodecName = "PCM signed 16-bit little-endian";
constexpr AVCodecID kDeliveryAudioCodecId = AV_CODEC_ID_OPUS;
constexpr AVSampleFormat kDeliveryAudioSampleFormat = AV_SAMPLE_FMT_S16;
constexpr const char* kDeliveryAudioCodecName = "Opus";

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

struct BufferRefCloser final {
  void operator()(AVBufferRef* buffer) const noexcept {
    av_buffer_unref(&buffer);
  }
};

using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefCloser>;

struct HardwareEncoderSelection final {
  const AVCodec* encoder{nullptr};
  AVHWDeviceType device_type{AV_HWDEVICE_TYPE_NONE};
  AVPixelFormat hardware_pixel_format{AV_PIX_FMT_NONE};
};

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
  bool creator_delivery;
};

[[nodiscard]] PresetConfiguration configuration_for(const VideoPreset preset) {
  switch (preset) {
  case VideoPreset::Ffv1Matroska:
    return {.muxer_name = "matroska",
            .container_name = "Matroska",
            .codec_name = "FFV1",
            .codec_id = AV_CODEC_ID_FFV1,
            .pixel_format = AV_PIX_FMT_YUV444P10LE,
            .lossless = true,
            .creator_delivery = false};
  case VideoPreset::ProRes422HqMov:
    return {.muxer_name = "mov",
            .container_name = "QuickTime / MOV",
            .codec_name = "Apple ProRes 422 HQ",
            .codec_id = AV_CODEC_ID_PRORES,
            .pixel_format = AV_PIX_FMT_YUV422P10LE,
            .lossless = false,
            .creator_delivery = false};
  case VideoPreset::Vp9OpusWebm:
    return {.muxer_name = "webm",
            .container_name = "WebM",
            .codec_name = "VP9",
            .codec_id = AV_CODEC_ID_VP9,
            .pixel_format = AV_PIX_FMT_YUV420P,
            .lossless = false,
            .creator_delivery = true};
  }
  throw std::invalid_argument("unknown export preset");
}

[[nodiscard]] const AVCodec* encoder_for(const VideoPreset preset) noexcept {
  if (preset == VideoPreset::ProRes422HqMov) {
    if (const AVCodec* encoder = avcodec_find_encoder_by_name("prores_ks"); encoder != nullptr) {
      return encoder;
    }
  }
  if (preset == VideoPreset::Vp9OpusWebm) {
    if (const AVCodec* encoder = avcodec_find_encoder_by_name("libvpx-vp9"); encoder != nullptr) {
      return encoder;
    }
  }
  const AVCodecID codec_id =
      preset == VideoPreset::Ffv1Matroska
          ? AV_CODEC_ID_FFV1
          : (preset == VideoPreset::ProRes422HqMov ? AV_CODEC_ID_PRORES : AV_CODEC_ID_VP9);
  return avcodec_find_encoder(codec_id);
}

[[nodiscard]] std::optional<HardwareEncoderSelection> hardware_vp9_encoder() noexcept {
#ifdef _WIN32
  constexpr const char* kEncoderName = "vp9_qsv";
  constexpr AVHWDeviceType kDeviceType = AV_HWDEVICE_TYPE_QSV;
  constexpr AVPixelFormat kHardwareFormat = AV_PIX_FMT_QSV;
#elif defined(__linux__)
  constexpr const char* kEncoderName = "vp9_vaapi";
  constexpr AVHWDeviceType kDeviceType = AV_HWDEVICE_TYPE_VAAPI;
  constexpr AVPixelFormat kHardwareFormat = AV_PIX_FMT_VAAPI;
#else
  return std::nullopt;
#endif
  const AVCodec* encoder = avcodec_find_encoder_by_name(kEncoderName);
  if (encoder == nullptr) {
    return std::nullopt;
  }
  return HardwareEncoderSelection{
      .encoder = encoder, .device_type = kDeviceType, .hardware_pixel_format = kHardwareFormat};
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

[[nodiscard]] ExportOutcome
failure(const ExportErrorCode code, std::string message,
        std::optional<audio_render::AudioRenderError> audio_error = std::nullopt) {
  return ExportOutcome::failure(
      {.code = code, .message = std::move(message), .audio_render_error = std::move(audio_error)});
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

[[nodiscard]] std::uint8_t quantize_luma_8(const double value) noexcept {
  return static_cast<std::uint8_t>(std::floor(16.0 + (219.0 * std::clamp(value, 0.0, 1.0)) + 0.5));
}

[[nodiscard]] std::uint8_t quantize_chroma_8(const double value) noexcept {
  return static_cast<std::uint8_t>(
      std::floor(128.0 + (224.0 * std::clamp(value, -0.5, 0.5)) + 0.5));
}

void write_16_bit_sample(std::uint8_t* destination, const std::uint16_t value) noexcept {
  AV_WL16(destination, value);
}

[[nodiscard]] std::int16_t quantize_audio_sample(const float input) noexcept {
  const double value = std::clamp(static_cast<double>(input), -1.0, 1.0);
  if (value <= -1.0) {
    return std::numeric_limits<std::int16_t>::min();
  }
  if (value >= 1.0) {
    return std::numeric_limits<std::int16_t>::max();
  }
  const double scaled = value * static_cast<double>(std::numeric_limits<std::int16_t>::max());
  const double rounded = scaled < 0.0 ? std::ceil(scaled - 0.5) : std::floor(scaled + 0.5);
  return static_cast<std::int16_t>(rounded);
}

[[nodiscard]] bool convert_audio_block(const audio::AudioBlock& source, AVFrame& destination,
                                       const std::int64_t expected_start,
                                       const std::size_t expected_samples, std::string& message) {
  if (source.format().sample_rate != audio_render::kTimelineAudioSampleRate ||
      source.format().channels != audio_render::kTimelineAudioChannels ||
      source.start_sample() != expected_start || source.frame_count() != expected_samples) {
    message = "audio renderer returned a block with an unexpected format or range";
    return false;
  }
  if (expected_samples > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    message = "audio block is too large for FFmpeg";
    return false;
  }
  const int writable = av_frame_make_writable(&destination);
  if (writable < 0) {
    message = "could not make audio encoder frame writable: " + ffmpeg_error(writable);
    return false;
  }
  destination.nb_samples = static_cast<int>(expected_samples);
  destination.pts = expected_start;
  destination.duration = static_cast<std::int64_t>(expected_samples);
  const auto left = source.channel(0);
  const auto right = source.channel(1);
  auto* output = destination.data[0];
  for (std::size_t index = 0; index < expected_samples; ++index) {
    if (!std::isfinite(left[index]) || !std::isfinite(right[index])) {
      message = "audio renderer returned a non-finite sample";
      return false;
    }
    write_16_bit_sample(output + (index * 4U),
                        static_cast<std::uint16_t>(quantize_audio_sample(left[index])));
    write_16_bit_sample(output + (index * 4U) + 2U,
                        static_cast<std::uint16_t>(quantize_audio_sample(right[index])));
  }
  return true;
}

[[nodiscard]] bool convert_frame(const render::VideoFrame& source, AVFrame& destination,
                                 std::string& message) {
  if (source.layout != render::PixelLayout::RgbaFloat32 ||
      !std::holds_alternative<std::shared_ptr<const render::CpuFrame>>(source.storage)) {
    message = "renderer did not return a CPU linear RGBA frame";
    return false;
  }
  const auto& cpu = std::get<std::shared_ptr<const render::CpuFrame>>(source.storage);
  if (!cpu || cpu->width() <= 0 || cpu->height() <= 0) {
    message = "renderer returned a frame with invalid dimensions";
    return false;
  }
  const auto writable = av_frame_make_writable(&destination);
  if (writable < 0) {
    message = "could not make encoder frame writable: " + ffmpeg_error(writable);
    return false;
  }

  const bool yuv420 = destination.format == AV_PIX_FMT_YUV420P;
  const bool nv12 = destination.format == AV_PIX_FMT_NV12;
  const bool subsampled = destination.format == AV_PIX_FMT_YUV422P10LE;
  if (!yuv420 && !nv12 && !subsampled && destination.format != AV_PIX_FMT_YUV444P10LE) {
    message = "unsupported export pixel format";
    return false;
  }
  const int source_width = cpu->width();
  const int source_height = cpu->height();
  const int content_width = std::max(
      1, std::min(destination.width,
                  static_cast<int>(std::lround(
                      static_cast<double>(source_width) *
                      std::min(static_cast<double>(destination.width) / source_width,
                               static_cast<double>(destination.height) / source_height)))));
  const int content_height = std::max(
      1, std::min(destination.height,
                  static_cast<int>(std::lround(
                      static_cast<double>(source_height) *
                      std::min(static_cast<double>(destination.width) / source_width,
                               static_cast<double>(destination.height) / source_height)))));
  const int offset_x = (destination.width - content_width) / 2;
  const int offset_y = (destination.height - content_height) / 2;
  const auto sample_at = [&](const int x, const int y) {
    if (x < offset_x || y < offset_y || x >= offset_x + content_width ||
        y >= offset_y + content_height) {
      return YuvSample{.y = 0.0, .cb = 0.0, .cr = 0.0};
    }
    const int source_x =
        std::clamp((x - offset_x) * source_width / content_width, 0, source_width - 1);
    const int source_y =
        std::clamp((y - offset_y) * source_height / content_height, 0, source_height - 1);
    return to_rec709_yuv(cpu->pixel(source_x, source_y));
  };
  for (int y = 0; y < destination.height; ++y) {
    auto* luma_row =
        destination.data[0] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[0]);
    for (int x = 0; x < destination.width; ++x) {
      const auto sample = sample_at(x, y);
      if (yuv420 || nv12) {
        luma_row[x] = quantize_luma_8(sample.y);
      } else {
        write_16_bit_sample(luma_row + (static_cast<std::ptrdiff_t>(x) * 2),
                            quantize_luma(sample.y));
      }
    }

    if (yuv420 || nv12) {
      if ((y % 2) == 0) {
        auto* cb_next =
            destination.data[1] + (static_cast<std::ptrdiff_t>(y / 2) * destination.linesize[1]);
        auto* cr_next = destination.data[2] == nullptr
                            ? nullptr
                            : destination.data[2] +
                                  (static_cast<std::ptrdiff_t>(y / 2) * destination.linesize[2]);
        for (int x = 0; x < destination.width; x += 2) {
          const auto top_left = sample_at(x, y);
          const auto top_right = sample_at(std::min(x + 1, destination.width - 1), y);
          const auto bottom_left = sample_at(x, std::min(y + 1, destination.height - 1));
          const auto bottom_right = sample_at(std::min(x + 1, destination.width - 1),
                                              std::min(y + 1, destination.height - 1));
          const auto cb = (top_left.cb + top_right.cb + bottom_left.cb + bottom_right.cb) / 4.0;
          const auto cr = (top_left.cr + top_right.cr + bottom_left.cr + bottom_right.cr) / 4.0;
          if (nv12) {
            cb_next[x] = quantize_chroma_8(cb);
            cb_next[x + 1] = quantize_chroma_8(cr);
          } else {
            cb_next[x / 2] = quantize_chroma_8(cb);
            cr_next[x / 2] = quantize_chroma_8(cr);
          }
        }
      }
      continue;
    }

    auto* cb_row = destination.data[1] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[1]);
    auto* cr_row = destination.data[2] + (static_cast<std::ptrdiff_t>(y) * destination.linesize[2]);
    if (subsampled) {
      for (int x = 0; x < destination.width; x += 2) {
        const auto left = sample_at(x, y);
        const auto right = sample_at(x + 1, y);
        const auto index = static_cast<std::ptrdiff_t>(x / 2) * 2;
        write_16_bit_sample(cb_row + index, quantize_chroma((left.cb + right.cb) * 0.5));
        write_16_bit_sample(cr_row + index, quantize_chroma((left.cr + right.cr) * 0.5));
      }
    } else {
      for (int x = 0; x < destination.width; ++x) {
        const auto sample = sample_at(x, y);
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

// Video draining needs to distinguish an encoder failure from a muxer/write
// failure. Only the former is eligible for the hardware-to-software retry;
// retrying a broken destination or muxer would duplicate the entire export
// without fixing its actual cause.
struct VideoDrainResult final {
  int status{0};
  bool encoder_failure{false};
};

[[nodiscard]] VideoDrainResult drain_video_packets(AVCodecContext& codec, AVFormatContext& format,
                                                   AVStream& stream, AVPacket& packet) {
  while (true) {
    const int received = avcodec_receive_packet(&codec, &packet);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
      return {};
    }
    if (received < 0) {
      return {.status = received, .encoder_failure = true};
    }
    av_packet_rescale_ts(&packet, codec.time_base, stream.time_base);
    packet.stream_index = stream.index;
    const int written = av_interleaved_write_frame(&format, &packet);
    av_packet_unref(&packet);
    if (written < 0) {
      return {.status = written, .encoder_failure = false};
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

namespace testing {
void set_hardware_failure_injection(const HardwareFailureInjection injection) {
  g_hardware_failure_injection.store(static_cast<int>(injection), std::memory_order_relaxed);
}

bool convert_frame_for_testing(const render::VideoFrame& source, AVFrame& destination,
                               std::string& message) {
  return convert_frame(source, destination, message);
}
} // namespace testing

PresetInfo preset_info(const VideoPreset preset) {
  const auto configuration = configuration_for(preset);
  return {.preset = preset,
          .display_name = preset == VideoPreset::Ffv1Matroska
                              ? "FFV1 10-bit / Matroska (lossless)"
                              : (preset == VideoPreset::ProRes422HqMov
                                     ? "Apple ProRes 422 HQ / MOV"
                                     : "VP9 + Opus / WebM (creator delivery)"),
          .container = configuration.container_name,
          .codec = configuration.codec_name,
          .available =
              [preset, &configuration] {
                const AVCodec* encoder = encoder_for(preset);
                if (encoder == nullptr ||
                    !encoder_supports_pixel_format(*encoder, configuration.pixel_format)) {
                  return false;
                }
                if (preset == VideoPreset::Vp9OpusWebm) {
                  return avcodec_find_encoder_by_name("libopus") != nullptr ||
                         avcodec_find_encoder(AV_CODEC_ID_OPUS) != nullptr;
                }
                return true;
              }(),
          .lossless = configuration.lossless};
}

ExportOutcome export_video_impl(const ExportRequest& request, const bool use_hardware_encoder,
                                bool* hardware_started = nullptr) {
  try {
    if (hardware_started != nullptr) {
      *hardware_started = false;
    }
    const auto injection = static_cast<testing::HardwareFailureInjection>(
        g_hardware_failure_injection.load(std::memory_order_relaxed));
    if (request.cancellation.stop_requested()) {
      return failure(ExportErrorCode::Cancelled, "export was cancelled before it started");
    }
    if (use_hardware_encoder && request.preset == VideoPreset::Vp9OpusWebm &&
        (injection == testing::HardwareFailureInjection::HardwareEncode ||
         injection == testing::HardwareFailureInjection::HardwareThenSoftwareEncode)) {
      if (hardware_started != nullptr) {
        *hardware_started = true;
      }
      return failure(ExportErrorCode::HardwareEncoderFailed, "injected hardware encoder failure");
    }
    if (use_hardware_encoder && request.preset == VideoPreset::Vp9OpusWebm &&
        injection == testing::HardwareFailureInjection::HardwareRender) {
      if (hardware_started != nullptr) {
        *hardware_started = true;
      }
      return failure(ExportErrorCode::RenderFailed, "injected renderer failure");
    }
    if (!use_hardware_encoder && request.preset == VideoPreset::Vp9OpusWebm &&
        (injection == testing::HardwareFailureInjection::SoftwareEncode ||
         injection == testing::HardwareFailureInjection::HardwareThenSoftwareEncode)) {
      return failure(ExportErrorCode::EncodingFailed, "injected software encoder failure");
    }
    const bool audio_only = request.platform_preset == PlatformPreset::PodcastAudioOnly;
    if (request.renderer == nullptr && !audio_only) {
      return failure(ExportErrorCode::InvalidRequest, "export requires a CPU renderer");
    }
    if (request.destination.empty() || request.destination.filename().empty()) {
      return failure(ExportErrorCode::InvalidRequest, "export destination must name a file");
    }
    if (request.include_audio && request.audio_renderer == nullptr) {
      return failure(ExportErrorCode::AudioRendererRequired,
                     "audio-inclusive export requires a timeline audio renderer");
    }
    if (audio_only && !request.include_audio) {
      return failure(ExportErrorCode::InvalidRequest,
                     "podcast audio-only delivery requires audio export");
    }
    if (audio_only && (request.caption_mode == CaptionExportMode::BurnIn ||
                       request.caption_mode == CaptionExportMode::BurnInAndSidecar)) {
      return failure(ExportErrorCode::InvalidRequest,
                     "caption burn-in is not available for audio-only delivery");
    }
    if (request.preset != VideoPreset::Ffv1Matroska &&
        request.preset != VideoPreset::ProRes422HqMov &&
        request.preset != VideoPreset::Vp9OpusWebm) {
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

    const bool delivery_platform = request.platform_preset != PlatformPreset::ReferenceFfv1 &&
                                   request.platform_preset != PlatformPreset::ReferenceProRes;
    const VideoPreset effective_preset =
        delivery_platform ? VideoPreset::Vp9OpusWebm : request.preset;
    const auto platform = platform_preset_info(request.platform_preset);
    const auto configuration = configuration_for(effective_preset);
    const auto resolve_dimension = [](const std::uint32_t source, const std::uint32_t requested,
                                      const std::uint32_t platform_target) {
      return requested != 0U ? requested : (platform_target != 0U ? platform_target : source);
    };
    std::uint32_t output_width = resolve_dimension(sequence->width, request.override_width,
                                                   delivery_platform ? platform.target_width : 0U);
    std::uint32_t output_height = resolve_dimension(
        sequence->height, request.override_height, delivery_platform ? platform.target_height : 0U);
    if (request.override_width != 0U && request.override_height == 0U) {
      output_height = static_cast<std::uint32_t>(std::max<std::uint64_t>(
          1U,
          (static_cast<std::uint64_t>(sequence->height) * output_width + (sequence->width / 2U)) /
              sequence->width));
    } else if (request.override_height != 0U && request.override_width == 0U) {
      output_width = static_cast<std::uint32_t>(std::max<std::uint64_t>(
          1U,
          (static_cast<std::uint64_t>(sequence->width) * output_height + (sequence->height / 2U)) /
              sequence->height));
    }
    // VP9 4:2:0 requires even dimensions. The exact requested dimensions are
    // retained for reference masters; delivery dimensions round up by one.
    if (configuration.pixel_format == AV_PIX_FMT_YUV420P) {
      output_width += output_width % 2U;
      output_height += output_height % 2U;
    }
    if (output_width == 0 || output_height == 0 || output_width > kMaximumDimension ||
        output_height > kMaximumDimension) {
      return failure(ExportErrorCode::InvalidRequest,
                     "export dimensions are outside the supported range");
    }
    if (configuration.pixel_format == AV_PIX_FMT_YUV422P10LE && (output_width % 2U) != 0U) {
      return failure(ExportErrorCode::InvalidRequest,
                     "ProRes 4:2:2 export requires an even sequence width");
    }
    BufferRefPtr hardware_device;
    std::optional<HardwareEncoderSelection> hardware_selection;
    bool hardware_encoder_used = false;
    const AVCodec* encoder = audio_only ? nullptr : encoder_for(effective_preset);
    if (!audio_only && use_hardware_encoder && effective_preset == VideoPreset::Vp9OpusWebm) {
      if (const auto candidate = hardware_vp9_encoder(); candidate.has_value()) {
        AVBufferRef* raw_device = nullptr;
        if (av_hwdevice_ctx_create(&raw_device, candidate->device_type, nullptr, nullptr, 0) >= 0 &&
            raw_device != nullptr) {
          hardware_device.reset(raw_device);
          hardware_selection = candidate;
          encoder = candidate->encoder;
          hardware_encoder_used = true;
          if (hardware_started != nullptr) {
            *hardware_started = true;
          }
        }
      }
    }
    const AVPixelFormat encoder_pixel_format = hardware_encoder_used
                                                   ? hardware_selection->hardware_pixel_format
                                                   : configuration.pixel_format;
    if (!audio_only && encoder == nullptr) {
      return failure(ExportErrorCode::EncoderUnavailable,
                     std::string{configuration.codec_name} + " encoder is not available");
    }
    if (!audio_only && !encoder_supports_pixel_format(*encoder, encoder_pixel_format)) {
      return failure(hardware_encoder_used ? ExportErrorCode::HardwareEncoderFailed
                                           : ExportErrorCode::EncoderUnavailable,
                     std::string{configuration.codec_name} +
                         " encoder does not support the required pixel format");
    }

    const edit::Time duration = request.snapshot.duration();
    if (duration.isZero() || duration.isNegative()) {
      return failure(ExportErrorCode::InvalidRequest, "cannot export an empty sequence");
    }
    edit::Rate output_rate = sequence->frame_rate;
    if (request.override_frame_rate_num != 0U || request.override_frame_rate_den != 0U) {
      if (request.override_frame_rate_num == 0U || request.override_frame_rate_den == 0U) {
        return failure(ExportErrorCode::InvalidRequest,
                       "frame-rate override must provide numerator and denominator");
      }
      try {
        output_rate = edit::Rate(request.override_frame_rate_num, request.override_frame_rate_den);
      } catch (const std::exception& exception) {
        return failure(ExportErrorCode::InvalidRequest,
                       std::string{"invalid frame-rate override: "} + exception.what());
      }
    } else if (delivery_platform && platform.target_frame_rate_num != 0U) {
      output_rate = edit::Rate(platform.target_frame_rate_num, platform.target_frame_rate_den);
    }
    if (output_rate.numerator() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        output_rate.denominator() > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      return failure(ExportErrorCode::InvalidRequest,
                     "output frame rate cannot be represented by FFmpeg");
    }
    const std::int64_t signed_frame_count =
        audio_only ? 0 : output_rate.framesAt(duration, edit::RoundingMode::Ceil);
    if ((!audio_only && signed_frame_count <= 0) ||
        static_cast<std::uint64_t>(signed_frame_count) > kMaximumFrameCount) {
      return failure(ExportErrorCode::InvalidRequest, "sequence frame count is not exportable");
    }
    const auto frame_count = static_cast<std::uint64_t>(signed_frame_count);
    std::int64_t signed_audio_sample_count = 0;
    if (request.include_audio) {
      signed_audio_sample_count =
          duration.rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::Ceil)
              .value();
      if (signed_audio_sample_count <= 0) {
        return failure(ExportErrorCode::InvalidRequest,
                       "sequence audio sample count is not exportable");
      }
    }
    const auto audio_sample_count = static_cast<std::uint64_t>(signed_audio_sample_count);
    if (request.video_quality.has_value() &&
        (request.video_quality.value() < 0 || request.video_quality.value() > 63)) {
      return failure(ExportErrorCode::InvalidRequest, "VP9 video quality must be between 0 and 63");
    }
    if (request.override_video_bitrate >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return failure(ExportErrorCode::InvalidRequest, "video bitrate is too large");
    }

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

    AVStream* video_stream = nullptr;
    CodecContextPtr video_codec;
    if (!audio_only) {
      video_stream = avformat_new_stream(format.get(), nullptr);
      if (video_stream == nullptr) {
        return failure(ExportErrorCode::EncodingFailed, "could not create export video stream");
      }

      video_codec.reset(avcodec_alloc_context3(encoder));
      if (!video_codec) {
        return failure(ExportErrorCode::EncodingFailed, "could not allocate video encoder");
      }
      video_codec->codec_id = configuration.codec_id;
      video_codec->codec_type = AVMEDIA_TYPE_VIDEO;
      video_codec->width = static_cast<int>(output_width);
      video_codec->height = static_cast<int>(output_height);
      video_codec->pix_fmt = hardware_encoder_used ? hardware_selection->hardware_pixel_format
                                                   : configuration.pixel_format;
      video_codec->time_base = {static_cast<int>(output_rate.denominator()),
                                static_cast<int>(output_rate.numerator())};
      video_codec->framerate = {static_cast<int>(output_rate.numerator()),
                                static_cast<int>(output_rate.denominator())};
      video_codec->gop_size = configuration.creator_delivery ? 60 : 1;
      video_codec->thread_count = configuration.creator_delivery ? 0 : 1;
      video_codec->flags |= AV_CODEC_FLAG_BITEXACT;
      video_codec->color_primaries = AVCOL_PRI_BT709;
      video_codec->color_trc = AVCOL_TRC_BT709;
      video_codec->colorspace = AVCOL_SPC_BT709;
      video_codec->color_range = AVCOL_RANGE_MPEG;
      video_codec->chroma_sample_location = configuration.pixel_format == AV_PIX_FMT_YUV422P10LE
                                                ? AVCHROMA_LOC_LEFT
                                                : AVCHROMA_LOC_UNSPECIFIED;
      if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        video_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      }
      if (effective_preset == VideoPreset::ProRes422HqMov) {
        video_codec->profile = AV_PROFILE_PRORES_HQ;
        video_codec->codec_tag = MKTAG('a', 'p', 'c', 'h');
        av_opt_set(video_codec->priv_data, "vendor", "apl0", 0);
      } else if (effective_preset == VideoPreset::Ffv1Matroska) {
        av_opt_set_int(video_codec->priv_data, "level", 3, 0);
        av_opt_set_int(video_codec->priv_data, "slicecrc", 1, 0);
      } else if (!hardware_encoder_used) {
        const auto info = platform_preset_info(request.platform_preset);
        const std::uint64_t bitrate = request.override_video_bitrate != 0U
                                          ? request.override_video_bitrate
                                          : info.target_video_bitrate;
        if (request.video_quality.has_value()) {
          av_opt_set_int(video_codec->priv_data, "crf", request.video_quality.value(), 0);
          video_codec->bit_rate = 0;
        } else if (bitrate != 0U) {
          video_codec->bit_rate = static_cast<std::int64_t>(bitrate);
        } else {
          av_opt_set_int(video_codec->priv_data, "crf", 32, 0);
          video_codec->bit_rate = 0;
        }
        av_opt_set(video_codec->priv_data, "deadline", "good", 0);
        av_opt_set_int(video_codec->priv_data, "cpu-used", 2, 0);
        av_opt_set_int(video_codec->priv_data, "row-mt", 1, 0);
      } else {
        const std::uint64_t bitrate = request.override_video_bitrate != 0U
                                          ? request.override_video_bitrate
                                          : platform.target_video_bitrate;
        if (bitrate != 0U) {
          video_codec->bit_rate = static_cast<std::int64_t>(bitrate);
        }
      }
      if (hardware_encoder_used && request.video_quality.has_value()) {
        // VAAPI/QSV expose quality under different option names across FFmpeg
        // builds. global_quality is the common constant-quality hint; a
        // rejected optional hint still falls back to the preset bitrate.
        if (av_opt_set_int(video_codec->priv_data, "global_quality", request.video_quality.value(),
                           0) >= 0) {
          video_codec->bit_rate = 0;
        }
      }

      BufferRefPtr hardware_frames;
      if (hardware_encoder_used) {
        hardware_frames.reset(av_hwframe_ctx_alloc(hardware_device.get()));
        if (!hardware_frames) {
          return failure(ExportErrorCode::HardwareEncoderFailed,
                         "could not allocate VP9 hardware frame context");
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(hardware_frames->data);
        frames->format = hardware_selection->hardware_pixel_format;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = video_codec->width;
        frames->height = video_codec->height;
        frames->initial_pool_size = 4;
        status = av_hwframe_ctx_init(hardware_frames.get());
        if (status < 0) {
          return failure(ExportErrorCode::HardwareEncoderFailed,
                         "could not initialize VP9 hardware frames: " + ffmpeg_error(status));
        }
        video_codec->hw_device_ctx = av_buffer_ref(hardware_device.get());
        video_codec->hw_frames_ctx = av_buffer_ref(hardware_frames.get());
        if (video_codec->hw_device_ctx == nullptr || video_codec->hw_frames_ctx == nullptr) {
          return failure(ExportErrorCode::HardwareEncoderFailed,
                         "could not attach VP9 hardware frames");
        }
      }

      status = avcodec_open2(video_codec.get(), encoder, nullptr);
      if (status < 0) {
        return failure(hardware_encoder_used ? ExportErrorCode::HardwareEncoderFailed
                                             : ExportErrorCode::EncodingFailed,
                       std::string{"could not open "} + configuration.codec_name +
                           " encoder: " + ffmpeg_error(status));
      }
      status = avcodec_parameters_from_context(video_stream->codecpar, video_codec.get());
      if (status < 0) {
        return failure(hardware_encoder_used ? ExportErrorCode::HardwareEncoderFailed
                                             : ExportErrorCode::EncodingFailed,
                       "could not configure export stream: " + ffmpeg_error(status));
      }
      video_stream->time_base = video_codec->time_base;
      video_stream->avg_frame_rate = video_codec->framerate;
      video_stream->r_frame_rate = video_codec->framerate;
    }

    AVStream* audio_stream = nullptr;
    CodecContextPtr audio_codec;
    const AVCodec* selected_audio_encoder = nullptr;
    if (request.include_audio) {
      if (configuration.creator_delivery) {
        selected_audio_encoder = avcodec_find_encoder_by_name("libopus");
        if (selected_audio_encoder == nullptr) {
          selected_audio_encoder = avcodec_find_encoder_by_name("opus");
        }
      } else {
        selected_audio_encoder = avcodec_find_encoder(kAudioCodecId);
      }
      if (selected_audio_encoder == nullptr) {
        return failure(ExportErrorCode::EncoderUnavailable,
                       std::string{configuration.creator_delivery ? kDeliveryAudioCodecName
                                                                  : kAudioCodecName} +
                           " encoder is not available");
      }
      audio_stream = avformat_new_stream(format.get(), nullptr);
      if (audio_stream == nullptr) {
        return failure(ExportErrorCode::EncodingFailed, "could not create export audio stream");
      }
      audio_codec.reset(avcodec_alloc_context3(selected_audio_encoder));
      if (!audio_codec) {
        return failure(ExportErrorCode::EncodingFailed, "could not allocate audio encoder");
      }
      audio_codec->codec_id =
          configuration.creator_delivery ? kDeliveryAudioCodecId : kAudioCodecId;
      audio_codec->codec_type = AVMEDIA_TYPE_AUDIO;
      audio_codec->sample_fmt =
          configuration.creator_delivery ? kDeliveryAudioSampleFormat : kAudioSampleFormat;
      audio_codec->sample_rate = static_cast<int>(audio_render::kTimelineAudioSampleRate);
      audio_codec->time_base = {1, static_cast<int>(audio_render::kTimelineAudioSampleRate)};
      const auto platform_audio_bitrate =
          platform_preset_info(request.platform_preset).target_audio_bitrate;
      audio_codec->bit_rate =
          configuration.creator_delivery
              ? static_cast<std::int64_t>(request.override_audio_bitrate != 0U
                                              ? request.override_audio_bitrate
                                              : platform_audio_bitrate)
              : static_cast<std::int64_t>(audio_render::kTimelineAudioSampleRate) *
                    audio_render::kTimelineAudioChannels * 16;
      audio_codec->bits_per_raw_sample = 16;
      audio_codec->thread_count = 1;
      audio_codec->flags |= AV_CODEC_FLAG_BITEXACT;
      av_channel_layout_default(&audio_codec->ch_layout,
                                static_cast<int>(audio_render::kTimelineAudioChannels));
      if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        audio_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      }
      status = avcodec_open2(audio_codec.get(), selected_audio_encoder, nullptr);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed, std::string{"could not open "} +
                                                            kAudioCodecName +
                                                            " encoder: " + ffmpeg_error(status));
      }
      status = avcodec_parameters_from_context(audio_stream->codecpar, audio_codec.get());
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not configure export audio stream: " + ffmpeg_error(status));
      }
      audio_stream->time_base = audio_codec->time_base;
    }

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

    FramePtr video_frame;
    FramePtr hardware_frame;
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
      return failure(ExportErrorCode::EncodingFailed,
                     "could not allocate FFmpeg export frame buffers");
    }
    if (!audio_only) {
      video_frame.reset(av_frame_alloc());
      if (!video_frame) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not allocate FFmpeg export video frame");
      }
      video_frame->format = hardware_encoder_used ? AV_PIX_FMT_NV12 : configuration.pixel_format;
      video_frame->width = video_codec->width;
      video_frame->height = video_codec->height;
      video_frame->color_primaries = AVCOL_PRI_BT709;
      video_frame->color_trc = AVCOL_TRC_BT709;
      video_frame->colorspace = AVCOL_SPC_BT709;
      video_frame->color_range = AVCOL_RANGE_MPEG;
      video_frame->chroma_location = video_codec->chroma_sample_location;
      status = av_frame_get_buffer(video_frame.get(), 32);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not allocate encoder pixels: " + ffmpeg_error(status));
      }
      if (hardware_encoder_used) {
        hardware_frame.reset(av_frame_alloc());
        if (!hardware_frame) {
          return failure(ExportErrorCode::HardwareEncoderFailed,
                         "could not allocate VP9 hardware frame");
        }
        hardware_frame->format = hardware_selection->hardware_pixel_format;
        hardware_frame->width = video_codec->width;
        hardware_frame->height = video_codec->height;
      }
    }

    FramePtr audio_frame;
    if (request.include_audio) {
      audio_frame.reset(av_frame_alloc());
      if (!audio_frame) {
        return failure(ExportErrorCode::EncodingFailed, "could not allocate audio encoder frame");
      }
      audio_frame->format = audio_codec->sample_fmt;
      audio_frame->sample_rate = audio_codec->sample_rate;
      audio_frame->nb_samples = static_cast<int>(kAudioBlockSamples);
      status = av_channel_layout_copy(&audio_frame->ch_layout, &audio_codec->ch_layout);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not configure audio encoder channels: " + ffmpeg_error(status));
      }
      status = av_frame_get_buffer(audio_frame.get(), 0);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not allocate audio encoder samples: " + ffmpeg_error(status));
      }
    }

    static std::atomic<std::uint64_t> next_epoch{1};
    const std::uint64_t epoch = next_epoch.fetch_add(1, std::memory_order_relaxed);
    if (!audio_only) {
      request.renderer->begin_epoch(epoch);
    }
    constexpr render::PreviewProfile full_quality{.scale = render::PreviewScale::Full,
                                                  .bypass_expensive_effects = false,
                                                  .use_proxies = false};

    std::int64_t next_audio_sample = 0;
    const auto encode_audio_through =
        [&](const std::int64_t exclusive_end,
            const bool flush_partial_block) -> std::optional<ExportError> {
      while (next_audio_sample < exclusive_end) {
        if (request.cancellation.stop_requested()) {
          return ExportError{.code = ExportErrorCode::Cancelled,
                             .message = "export was cancelled",
                             .audio_render_error = std::nullopt};
        }
        const auto remaining = static_cast<std::uint64_t>(exclusive_end - next_audio_sample);
        if (!flush_partial_block && remaining < kAudioBlockSamples) {
          break;
        }
        const auto block_samples = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(kAudioBlockSamples)));
        auto rendered = request.audio_renderer->render(request.snapshot,
                                                       {.start_sample = next_audio_sample,
                                                        .sample_count = block_samples,
                                                        .cancellation = request.cancellation});
        if (!rendered) {
          const auto& audio_error = rendered.error();
          const bool cancelled = audio_error.code == audio_render::AudioRenderErrorCode::Cancelled;
          return ExportError{.code = cancelled ? ExportErrorCode::Cancelled
                                               : ExportErrorCode::AudioRenderFailed,
                             .message = std::string{cancelled ? "audio export was cancelled: "
                                                              : "could not render export audio: "} +
                                        audio_error.message,
                             .audio_render_error = audio_error};
        }
        std::string conversion_error;
        if (!convert_audio_block(rendered.value(), *audio_frame, next_audio_sample, block_samples,
                                 conversion_error)) {
          return ExportError{.code = ExportErrorCode::AudioRenderFailed,
                             .message = std::move(conversion_error),
                             .audio_render_error = std::nullopt};
        }
        status = avcodec_send_frame(audio_codec.get(), audio_frame.get());
        if (status < 0) {
          return ExportError{.code = ExportErrorCode::EncodingFailed,
                             .message = "could not submit export audio: " + ffmpeg_error(status),
                             .audio_render_error = std::nullopt};
        }
        status = drain_packets(*audio_codec, *format, *audio_stream, *packet);
        if (status < 0) {
          return ExportError{.code = ExportErrorCode::EncodingFailed,
                             .message = "could not encode export audio: " + ffmpeg_error(status),
                             .audio_render_error = std::nullopt};
        }
        next_audio_sample += static_cast<std::int64_t>(block_samples);
      }
      return std::nullopt;
    };

    if (!audio_only) {
      for (std::uint64_t index = 0; index < frame_count; ++index) {
        if (request.cancellation.stop_requested()) {
          return failure(ExportErrorCode::Cancelled, "export was cancelled");
        }
        const edit::Time timeline_time = output_rate.frameTime(static_cast<std::int64_t>(index));
        auto rendered =
            request.renderer->request_frame(request.snapshot, timeline_time, full_quality, epoch);
        if (!rendered) {
          return failure(ExportErrorCode::RenderFailed, "could not render export frame " +
                                                            std::to_string(index) + ": " +
                                                            rendered.error->message);
        }
        render::VideoFrame frame_for_export = *rendered.value;
        if (request.caption_mode == CaptionExportMode::BurnIn ||
            request.caption_mode == CaptionExportMode::BurnInAndSidecar) {
          if (frame_for_export.layout != render::PixelLayout::RgbaFloat32 ||
              !std::holds_alternative<std::shared_ptr<const render::CpuFrame>>(
                  frame_for_export.storage)) {
            return failure(ExportErrorCode::RenderFailed,
                           "renderer did not return a CPU frame for caption burn-in");
          }
          const auto& source_cpu =
              std::get<std::shared_ptr<const render::CpuFrame>>(frame_for_export.storage);
          if (!source_cpu) {
            return failure(ExportErrorCode::RenderFailed,
                           "renderer returned an empty frame for caption burn-in");
          }
          auto writable_cpu = std::make_shared<render::CpuFrame>(*source_cpu);
          if (const auto burn_in_error =
                  burn_in_captions(*writable_cpu, request.captions, timeline_time)) {
            return failure(ExportErrorCode::RenderFailed,
                           "could not burn in captions (error " +
                               std::to_string(static_cast<int>(*burn_in_error)) + ")");
          }
          frame_for_export.storage =
              std::shared_ptr<const render::CpuFrame>(std::move(writable_cpu));
        }
        std::string conversion_error;
        if (!convert_frame(frame_for_export, *video_frame, conversion_error)) {
          return failure(ExportErrorCode::RenderFailed, std::move(conversion_error));
        }
        video_frame->pts = static_cast<std::int64_t>(index);
        video_frame->duration = 1;
        AVFrame* frame_to_encode = video_frame.get();
        if (hardware_encoder_used) {
          av_frame_unref(hardware_frame.get());
          hardware_frame->format = hardware_selection->hardware_pixel_format;
          hardware_frame->width = video_codec->width;
          hardware_frame->height = video_codec->height;
          status = av_hwframe_get_buffer(video_codec->hw_frames_ctx, hardware_frame.get(), 0);
          if (status < 0) {
            return failure(ExportErrorCode::HardwareEncoderFailed,
                           "could not acquire a VP9 hardware frame: " + ffmpeg_error(status));
          }
          status = av_hwframe_transfer_data(hardware_frame.get(), video_frame.get(), 0);
          if (status < 0) {
            return failure(ExportErrorCode::HardwareEncoderFailed,
                           "could not upload frame to VP9 hardware encoder: " +
                               ffmpeg_error(status));
          }
          hardware_frame->pts = video_frame->pts;
          hardware_frame->duration = video_frame->duration;
          hardware_frame->color_primaries = video_frame->color_primaries;
          hardware_frame->color_trc = video_frame->color_trc;
          hardware_frame->colorspace = video_frame->colorspace;
          hardware_frame->color_range = video_frame->color_range;
          frame_to_encode = hardware_frame.get();
        }
        status = avcodec_send_frame(video_codec.get(), frame_to_encode);
        if (status < 0) {
          return failure(hardware_encoder_used ? ExportErrorCode::HardwareEncoderFailed
                                               : ExportErrorCode::EncodingFailed,
                         "could not submit export frame: " + ffmpeg_error(status));
        }
        const auto video_drain = drain_video_packets(*video_codec, *format, *video_stream, *packet);
        if (video_drain.status < 0) {
          const auto error_code = hardware_encoder_used && video_drain.encoder_failure
                                      ? ExportErrorCode::HardwareEncoderFailed
                                      : ExportErrorCode::EncodingFailed;
          return failure(error_code, std::string{video_drain.encoder_failure
                                                     ? "could not encode export frame: "
                                                     : "could not write export frame: "} +
                                         ffmpeg_error(video_drain.status));
        }
        if (request.include_audio) {
          const edit::Time completed_frame_time =
              output_rate.frameTime(static_cast<std::int64_t>(index + 1U));
          const std::int64_t frame_audio_end =
              completed_frame_time
                  .rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::Ceil)
                  .value();
          const std::int64_t audio_end = std::min(frame_audio_end, signed_audio_sample_count);
          if (auto audio_error = encode_audio_through(audio_end, false)) {
            return ExportOutcome::failure(std::move(*audio_error));
          }
        }
        if (auto callback_error =
                report_progress(request, index + 1U, frame_count, duration, output_rate)) {
          return ExportOutcome::failure(std::move(*callback_error));
        }
        if (request.cancellation.stop_requested()) {
          return failure(ExportErrorCode::Cancelled, "export was cancelled");
        }
      }
    }

    if (!audio_only) {
      status = avcodec_send_frame(video_codec.get(), nullptr);
      if (status < 0) {
        return failure(hardware_encoder_used ? ExportErrorCode::HardwareEncoderFailed
                                             : ExportErrorCode::EncodingFailed,
                       "could not flush video encoder: " + ffmpeg_error(status));
      }
      const auto video_drain = drain_video_packets(*video_codec, *format, *video_stream, *packet);
      if (video_drain.status < 0) {
        const auto error_code = hardware_encoder_used && video_drain.encoder_failure
                                    ? ExportErrorCode::HardwareEncoderFailed
                                    : ExportErrorCode::EncodingFailed;
        return failure(error_code,
                       std::string{video_drain.encoder_failure ? "could not finish video encoding: "
                                                               : "could not write video packet: "} +
                           ffmpeg_error(video_drain.status));
      }
    }
    if (request.include_audio) {
      if (auto audio_error = encode_audio_through(signed_audio_sample_count, true)) {
        return ExportOutcome::failure(std::move(*audio_error));
      }
      status = avcodec_send_frame(audio_codec.get(), nullptr);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not flush audio encoder: " + ffmpeg_error(status));
      }
      status = drain_packets(*audio_codec, *format, *audio_stream, *packet);
      if (status < 0) {
        return failure(ExportErrorCode::EncodingFailed,
                       "could not finish audio encoding: " + ffmpeg_error(status));
      }
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
        .preset = effective_preset,
        .container = configuration.container_name,
        .video_codec = audio_only ? "" : configuration.codec_name,
        .video_encoder = audio_only || encoder == nullptr ? "" : encoder->name,
        .audio_codec =
            request.include_audio
                ? (configuration.creator_delivery ? kDeliveryAudioCodecName : kAudioCodecName)
                : "",
        .audio_encoder = request.include_audio && selected_audio_encoder != nullptr
                             ? selected_audio_encoder->name
                             : "",
        .frame_count = frame_count,
        .audio_sample_count = audio_sample_count,
        .source_timeline_duration = duration,
        .encoded_video_duration =
            audio_only ? edit::Time{} : output_rate.frameTime(signed_frame_count),
        .encoded_audio_duration =
            request.include_audio
                ? edit::Time(signed_audio_sample_count, audio_render::kTimelineAudioSampleRate)
                : edit::Time{},
        .video_exported = !audio_only,
        .audio_exported = request.include_audio,
        .hardware_encoder_used = hardware_encoder_used,
        .caption_sidecar_path = {},
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

    // Write caption sidecar after the media file is committed so a sidecar
    // failure never prevents the media file from being available. The sidecar
    // path is derived from the destination stem.
    if (request.caption_mode == CaptionExportMode::Sidecar ||
        request.caption_mode == CaptionExportMode::BurnInAndSidecar) {
      const auto sidecar_outcome = write_caption_sidecar(
          request.captions, request.destination, request.sidecar_format, edit::Time{}, duration);
      if (sidecar_outcome) {
        completed_export.caption_sidecar_path = sidecar_outcome.value().path;
      }
      // Sidecar failures are non-fatal: the media file is already committed.
      // A missing sidecar is reported by an empty caption_sidecar_path.
    }

    return ExportOutcome::success(std::move(completed_export));
  } catch (const std::exception& exception) {
    return failure(ExportErrorCode::EncodingFailed,
                   std::string{"unexpected export failure: "} + exception.what());
  } catch (...) {
    return failure(ExportErrorCode::EncodingFailed, "unexpected export failure");
  }
}

ExportOutcome export_video(const ExportRequest& request) {
  bool hardware_started = false;
  ExportOutcome outcome =
      export_video_impl(request, request.prefer_hardware_encoder, &hardware_started);
  if (outcome || !hardware_started || !request.prefer_hardware_encoder ||
      (request.preset != VideoPreset::Vp9OpusWebm &&
       request.platform_preset == PlatformPreset::ReferenceFfv1) ||
      request.platform_preset == PlatformPreset::ReferenceProRes ||
      request.platform_preset == PlatformPreset::PodcastAudioOnly ||
      request.cancellation.stop_requested()) {
    return outcome;
  }
  const auto code = outcome.error().code;
  if (code != ExportErrorCode::HardwareEncoderFailed) {
    return outcome;
  }
  // A hardware device can disappear or reject an otherwise valid frame after
  // initialization. Restarting the complete encode against a new temporary
  // sibling is the only safe way to switch codecs without corrupting WebM.
  ExportRequest software_request = request;
  software_request.prefer_hardware_encoder = false;
  if (request.progress) {
    try {
      request.progress({.completed_frames = 0,
                        .total_frames = 0,
                        .timeline_time = {},
                        .fraction = 0.0,
                        .restarted_after_hardware_fallback = true});
    } catch (const std::exception& exception) {
      return failure(
          ExportErrorCode::ProgressCallbackFailed,
          std::string{"export progress callback failed: " + std::string(exception.what())});
    } catch (...) {
      return failure(ExportErrorCode::ProgressCallbackFailed, "export progress callback failed");
    }
  }
  return export_video_impl(software_request, false, nullptr);
}

} // namespace video_editor::export_service

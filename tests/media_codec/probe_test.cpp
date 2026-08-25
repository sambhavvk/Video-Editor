// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/dependency_versions.h"
#include "video_editor/media_codec/format_open.h"
#include "video_editor/media_codec/probe.h"
#include "video_editor/media_codec/runtime.h"

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::media {
namespace {

void write_u16(std::ostream& output, const std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                  static_cast<char>((value >> 8U) & 0xFFU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& output, const std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
      static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path write_test_wave() {
  const auto path = std::filesystem::temp_directory_path() / "video_editor_probe_test.wav";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  constexpr std::uint32_t sample_rate = 48'000;
  constexpr std::uint16_t channels = 2;
  constexpr std::uint16_t bits = 16;
  constexpr std::uint32_t frames = 480;
  constexpr std::uint32_t data_size = frames * channels * (bits / 8U);

  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 1);
  write_u16(output, channels);
  write_u32(output, sample_rate);
  write_u32(output, sample_rate * channels * (bits / 8U));
  write_u16(output, channels * (bits / 8U));
  write_u16(output, bits);
  output.write("data", 4);
  write_u32(output, data_size);
  const std::array<char, data_size> silence{};
  output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
  return path;
}

[[nodiscard]] std::string shell_quote(const std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_media_codec_" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

class LogCapture final {
public:
  void append(const int level, const char* message) {
    if (level > AV_LOG_WARNING || message == nullptr) {
      return;
    }
    messages_.emplace_back(message);
  }

  [[nodiscard]] const std::vector<std::string>& messages() const noexcept { return messages_; }

  [[nodiscard]] bool contains(const std::string_view needle) const {
    for (const std::string& message : messages_) {
      if (message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

private:
  std::vector<std::string> messages_;
};

LogCapture* active_log_capture = nullptr;

void capture_av_log(void* opaque, const int level, const char* fmt, va_list args) {
  static_cast<void>(opaque);
  LogCapture* capture = active_log_capture;
  if (capture == nullptr || fmt == nullptr) {
    return;
  }
  std::array<char, 2048> buffer{};
  va_list copied;
  va_copy(copied, args);
  const int written = vsnprintf(buffer.data(), buffer.size(), fmt, copied);
  va_end(copied);
  if (written <= 0) {
    return;
  }
  capture->append(level & 0xff, buffer.data());
}

class LogCaptureGuard final {
public:
  explicit LogCaptureGuard(LogCapture& capture) {
    active_log_capture = &capture;
    av_log_set_callback(capture_av_log);
  }

  LogCaptureGuard(const LogCaptureGuard&) = delete;
  LogCaptureGuard& operator=(const LogCaptureGuard&) = delete;

  ~LogCaptureGuard() {
    av_log_set_callback(av_log_default_callback);
    active_log_capture = nullptr;
  }
};

[[nodiscard]] bool create_attachment_fixture(const std::filesystem::path& attachment,
                                             const std::filesystem::path& output) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  static_cast<void>(attachment);
  static_cast<void>(output);
  return false;
#else
  const std::vector<std::string> arguments{
      VIDEO_EDITOR_TEST_FFMPEG,
      "-hide_banner",
      "-loglevel",
      "error",
      "-nostdin",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=16x16:rate=1:duration=1",
      "-c:v",
      "mpeg4",
      "-q:v",
      "8",
      "-attach",
      attachment.string(),
      "-metadata:s:t",
      "mimetype=application/octet-stream",
      output.string(),
  };
  std::ostringstream command;
  for (const std::string& argument : arguments) {
    if (command.tellp() > 0) {
      command << ' ';
    }
    command << shell_quote(argument);
  }
  return std::system(command.str().c_str()) == 0;
#endif
}

TEST(MediaFormatOpen, SuppressesHarmlessDecodeNoise) {
  EXPECT_TRUE(should_suppress_ffmpeg_log(
      AV_LOG_WARNING, "[opus @ 0x1] Could not update timestamps for skipped samples.\n"));
  EXPECT_TRUE(should_suppress_ffmpeg_log(
      AV_LOG_WARNING, "[swscaler @ 0x1] deprecated pixel format used, make sure you did set range correctly\n"));
  EXPECT_FALSE(should_suppress_ffmpeg_log(AV_LOG_ERROR, "Read error at pos. 190049971"));
  EXPECT_FALSE(should_suppress_ffmpeg_log(AV_LOG_WARNING, "Could not find codec parameters"));
  EXPECT_FALSE(should_suppress_ffmpeg_log(AV_LOG_INFO, "Could not update timestamps for skipped samples"));
}

TEST(MediaFormatOpen, ApplyInputProbeOptionsWritesDefaults) {
  AVFormatContext* context = avformat_alloc_context();
  ASSERT_NE(context, nullptr);
  apply_input_probe_options(*context);
  EXPECT_EQ(context->probesize, 8 * 1024 * 1024);
  EXPECT_EQ(context->max_analyze_duration, 10 * 1000 * 1000);
  avformat_free_context(context);
}

TEST(MediaFormatOpen, SkipsAttachmentStreamAnalysisWarnings) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  GTEST_SKIP() << "ffmpeg is not available";
#else
  TemporaryDirectory directory;
  const std::filesystem::path attachment = directory.path() / "attachment.bin";
  {
    std::ofstream output(attachment, std::ios::binary | std::ios::trunc);
    output << "video-editor attachment fixture";
  }
  const std::filesystem::path fixture = directory.path() / "attachment.mkv";
  ASSERT_TRUE(create_attachment_fixture(attachment, fixture))
      << "failed to create matroska attachment fixture";

  LogCapture capture;
  LogCaptureGuard guard(capture);

  const auto probe_result = probe(fixture);
  ASSERT_TRUE(probe_result) << probe_result.error().message;

  AVFormatContext* raw_context = avformat_alloc_context();
  ASSERT_NE(raw_context, nullptr);
  apply_input_probe_options(*raw_context);
  ASSERT_GE(avformat_open_input(&raw_context, fixture.string().c_str(), nullptr, nullptr), 0);
  ASSERT_GE(inspect_input_streams(*raw_context), 0);

  bool discarded_attachment = false;
  for (unsigned index = 0; index < raw_context->nb_streams; ++index) {
    const AVStream* stream = raw_context->streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) {
      continue;
    }
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
      discarded_attachment = stream->discard == AVDISCARD_ALL;
      break;
    }
  }
  avformat_close_input(&raw_context);

  bool saw_attachment = false;
  for (const StreamDescriptor& stream : probe_result.value().streams) {
    if (stream.kind == StreamKind::Attachment) {
      saw_attachment = true;
      break;
    }
  }
  EXPECT_TRUE(saw_attachment);
  EXPECT_TRUE(discarded_attachment);
  std::string joined;
  for (const std::string& message : capture.messages()) {
    joined += message;
  }
  EXPECT_FALSE(capture.contains("Could not find codec parameters")) << joined;
  EXPECT_FALSE(capture.contains("analyzeduration")) << joined;
#endif
}

TEST(MediaRuntime, ReportsPinnedAbi) {
  const RuntimeInfo info = runtime_info();
  EXPECT_TRUE(info.expected_abi);
  EXPECT_EQ(info.avformat.major, dependency_versions::kAvformatMajor);
  EXPECT_EQ(info.avformat.minor, dependency_versions::kAvformatMinor);
  EXPECT_EQ(info.avformat.patch, dependency_versions::kAvformatPatch);
  EXPECT_EQ(info.avcodec.major, dependency_versions::kAvcodecMajor);
  EXPECT_EQ(info.avcodec.minor, dependency_versions::kAvcodecMinor);
  EXPECT_EQ(info.avcodec.patch, dependency_versions::kAvcodecPatch);
  EXPECT_EQ(info.avutil.major, dependency_versions::kAvutilMajor);
  EXPECT_EQ(info.avutil.minor, dependency_versions::kAvutilMinor);
  EXPECT_EQ(info.avutil.patch, dependency_versions::kAvutilPatch);
  EXPECT_EQ(info.swresample.major, dependency_versions::kSwresampleMajor);
  EXPECT_EQ(info.swresample.minor, dependency_versions::kSwresampleMinor);
  EXPECT_EQ(info.swresample.patch, dependency_versions::kSwresamplePatch);
  EXPECT_EQ(info.swscale.major, dependency_versions::kSwscaleMajor);
  EXPECT_EQ(info.swscale.minor, dependency_versions::kSwscaleMinor);
  EXPECT_EQ(info.swscale.patch, dependency_versions::kSwscalePatch);
  EXPECT_FALSE(info.configuration.empty());
  EXPECT_FALSE(info.license.empty());
}

TEST(MediaProbe, RejectsMissingFileWithoutThrowing) {
  const auto result = probe("/definitely/not/a/video-editor-test-file.mov");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, MediaErrorCode::FileNotFound);
}

TEST(MediaProbe, OpensNumberedImageSequencePatternUri) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("video_editor_probe_seq_" + std::to_string(std::rand()));
  std::filesystem::create_directories(directory);
  const auto write_ppm = [&](const std::string& name) {
    std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
    output << "P6\n2 2\n255\n";
    const char pixel[] = {static_cast<char>(255), 0, 0};
    for (int i = 0; i < 4; ++i) {
      output.write(pixel, 3);
    }
  };
  write_ppm("clip0001.ppm");
  write_ppm("clip0002.ppm");
  const auto result = probe(directory / "clip%04d.ppm");
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_GE(result.value().streams.size(), 1U);
  EXPECT_EQ(result.value().streams.front().kind, StreamKind::Video);
}

TEST(MediaProbe, DescribesEveryWaveStream) {
  const auto path = write_test_wave();
  const auto result = probe(path);
  std::filesystem::remove(path);

  ASSERT_TRUE(result) << result.error().message;
  const AssetDescriptor& asset = result.value();
  ASSERT_EQ(asset.streams.size(), 1U);
  EXPECT_EQ(asset.best_audio_stream, 0);
  EXPECT_EQ(asset.best_video_stream, -1);
  EXPECT_EQ(asset.streams.front().kind, StreamKind::Audio);
  ASSERT_TRUE(asset.streams.front().audio.has_value());
  EXPECT_EQ(asset.streams.front().audio->sample_rate, 48'000);
  EXPECT_EQ(asset.streams.front().audio->channels, 2);
  EXPECT_TRUE(asset.duration_microseconds.has_value());
}

} // namespace
} // namespace video_editor::media

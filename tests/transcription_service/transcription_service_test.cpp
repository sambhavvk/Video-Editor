// SPDX-License-Identifier: MPL-2.0
#include "video_editor/transcription_service/transcription_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace video_editor::transcription {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_transcription_test_" + std::to_string(++sequence_));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic_uint64_t sequence_{0};
  std::filesystem::path path_;
};

class FakeFetcher final : public ModelFetcher {
public:
  std::string bytes;
  int calls{0};
  bool fail{false};

  [[nodiscard]] Result<std::uintmax_t> fetch(const ModelDescriptor&,
                                             const std::filesystem::path& destination,
                                             std::stop_token) override {
    ++calls;
    if (fail) {
      return Result<std::uintmax_t>::failure(
          {ErrorCode::ModelDownloadFailed, 0, "fake fetch failed", true});
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return Result<std::uintmax_t>::success(bytes.size());
  }
};

class FakeDecoder final : public AudioDecoder {
public:
  std::size_t sample_count{16'000U};

  [[nodiscard]] Result<AudioData> decode(const std::filesystem::path&, const AudioRange& range,
                                         std::stop_token,
                                         const ProgressCallback& progress) override {
    last_range = range;
    if (progress)
      progress(1.0, "decoded");
    return Result<AudioData>::success(
        AudioData{.samples = std::vector<float>(sample_count), .sample_rate = 16'000U});
  }

  AudioRange last_range{};
};

class FakeBackend final : public TranscriptionBackend {
public:
  struct Word final {
    std::string text;
    std::int64_t start{0};
    std::int64_t end{0};
    float probability{1.0F};
  };

  std::vector<Word> words{{"hello", 10, 20, 0.8F}};
  std::string output_backend{"fake"};
  std::string capability_backend{"fake"};
  std::string detected_language;

  [[nodiscard]] BackendCapabilities capabilities() const override {
    return {.available = true, .vulkan_available = false, .backend = capability_backend};
  }

  [[nodiscard]] Result<ResultMessage> transcribe(const std::filesystem::path&, const AudioData&,
                                                 const OptionsMessage&, std::stop_token,
                                                 const ProgressCallback& progress) override {
    if (progress)
      progress(1.0, "inference");
    ResultMessage result;
    result.set_backend(output_backend);
    result.set_detected_language(detected_language);
    for (const auto& source : words) {
      auto* word = result.add_words();
      word->set_text(source.text);
      word->set_start_centiseconds(source.start);
      word->set_end_centiseconds(source.end);
      word->set_probability(source.probability);
    }
    return Result<ResultMessage>::success(std::move(result));
  }
};

void write_pcm16_wav(const std::filesystem::path& path, const std::size_t sample_count,
                     const std::uint32_t sample_rate = 16'000U) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output);
  const auto write_u16 = [&output](const std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
  };
  const auto write_u32 = [&output](const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      output.put(static_cast<char>((value >> shift) & 0xffU));
  };
  const auto data_bytes = static_cast<std::uint32_t>(sample_count * 2U);
  output.write("RIFF", 4);
  write_u32(36U + data_bytes);
  output.write("WAVEfmt ", 8);
  write_u32(16U);
  write_u16(1U);
  write_u16(1U);
  write_u32(sample_rate);
  write_u32(sample_rate * 2U);
  write_u16(2U);
  write_u16(16U);
  output.write("data", 4);
  write_u32(data_bytes);
  for (std::size_t index = 0; index < sample_count; ++index) {
    const auto value = static_cast<std::int16_t>(index % 20U);
    write_u16(static_cast<std::uint16_t>(value));
  }
}

[[nodiscard]] ModelDescriptor small_model(std::string bytes) {
  // SHA-1("test-model") = b416f3a5355826c4eee348620c47399844e87bcf.
  return {.id = "test",
          .filename = "test.bin",
          .url = "fake://test",
          .digest_algorithm = "sha1",
          .digest = "b416f3a5355826c4eee348620c47399844e87bcf",
          .expected_bytes = bytes.size()};
}

TEST(Options, RejectsUnknownSchemaAndUnsupportedModel) {
  OptionsMessage options;
  std::string diagnostic;
  EXPECT_FALSE(validate_options(options, diagnostic));
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("not-base");
  options.set_language("auto");
  EXPECT_FALSE(validate_options(options, diagnostic));
}

TEST(ModelManager, VerifiesAndCachesWithoutCallingFetcherTwice) {
  TemporaryDirectory directory;
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  auto descriptor = small_model(fetcher.bytes);
  ModelManager manager(directory.path(), fetcher, descriptor);

  EXPECT_FALSE(manager.verify());
  const auto first = manager.ensure();
  ASSERT_TRUE(first);
  EXPECT_EQ(fetcher.calls, 1);
  EXPECT_TRUE(manager.verify());
  const auto second = manager.ensure();
  ASSERT_TRUE(second);
  EXPECT_EQ(fetcher.calls, 1);
}

TEST(ModelManager, VerificationHonorsCancellationBeforeDigest) {
  TemporaryDirectory directory;
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  const auto descriptor = small_model(fetcher.bytes);
  const auto destination = directory.path() / descriptor.filename;
  std::ofstream(destination, std::ios::binary) << fetcher.bytes;
  ModelManager manager(directory.path(), fetcher, descriptor);
  std::stop_source cancellation;
  cancellation.request_stop();
  EXPECT_FALSE(manager.verify(cancellation.get_token()));
}

TEST(ModelManager, RejectsBadSizeAndChecksumAndLeavesNoPartialFile) {
  TemporaryDirectory directory;
  FakeFetcher fetcher;
  fetcher.bytes = "wrong";
  auto descriptor = small_model("test-model");
  ModelManager manager(directory.path(), fetcher, descriptor);
  const auto result = manager.ensure();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::ModelSizeMismatch);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path()),
                          std::filesystem::directory_iterator()),
            0);
}

TEST(ModelManager, RejectsChecksumMismatchAtExpectedSize) {
  TemporaryDirectory directory;
  FakeFetcher fetcher;
  fetcher.bytes = "xxxxxxxxxx";
  ModelManager manager(directory.path(), fetcher, small_model("test-model"));
  const auto result = manager.ensure();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::ModelChecksumMismatch);
}

TEST(ModelManager, ReplacesInvalidExistingCacheArtifact) {
  TemporaryDirectory directory;
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  const auto descriptor = small_model(fetcher.bytes);
  const auto destination = directory.path() / descriptor.filename;
  std::ofstream(destination, std::ios::binary) << "corrupt";
  ModelManager manager(directory.path(), fetcher, descriptor);
  ASSERT_TRUE(manager.ensure());
  EXPECT_TRUE(manager.verify());
  EXPECT_EQ(fetcher.calls, 1);
}

TEST(TranscriptionService, UsesInjectedBoundariesAndReportsProgress) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "audio.wav";
  std::ofstream(input) << "fixture";
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  auto descriptor = small_model(fetcher.bytes);
  ModelManager models(directory.path() / "models", fetcher, descriptor);
  FakeDecoder decoder;
  FakeBackend backend;
  TranscriptionService service(models, decoder, backend);
  OptionsMessage options;
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");
  std::vector<double> progress;
  const auto result = service.transcribe(
      std::filesystem::absolute(input), options, {},
      [&progress](const double value, std::string_view) { progress.push_back(value); });
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().backend(), "fake");
  EXPECT_EQ(result.value().model_digest_algorithm(), "sha1");
  EXPECT_EQ(result.value().words_size(), 1);
  EXPECT_EQ(result.value().words(0).start_centiseconds(), 10);
  EXPECT_EQ(result.value().words(0).end_centiseconds(), 20);
  ASSERT_FALSE(progress.empty());
  EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end()));
}

TEST(TranscriptionService, PassesSourceRangeAndReturnsSourceAbsoluteWords) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "audio.wav";
  std::ofstream(input) << "fixture";
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  ModelManager models(directory.path() / "models", fetcher, small_model(fetcher.bytes));
  FakeDecoder decoder;
  FakeBackend backend;
  TranscriptionService service(models, decoder, backend);
  OptionsMessage options;
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");
  options.set_source_start_centiseconds(125);
  options.set_source_duration_centiseconds(400);
  const auto result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_TRUE(result);
  EXPECT_EQ(decoder.last_range.start_centiseconds, 125);
  EXPECT_EQ(decoder.last_range.duration_centiseconds, 400);
  ASSERT_EQ(result.value().words_size(), 1);
  EXPECT_EQ(result.value().words(0).start_centiseconds(), 135);
  EXPECT_EQ(result.value().words(0).end_centiseconds(), 145);
  EXPECT_EQ(result.value().source_start_centiseconds(), 125);
  EXPECT_EQ(result.value().source_duration_centiseconds(), 400);
}

TEST(TranscriptionService, RejectsInvalidBackendWordRecords) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "audio.wav";
  std::ofstream(input) << "fixture";
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  ModelManager models(directory.path() / "models", fetcher, small_model(fetcher.bytes));
  FakeDecoder decoder;
  FakeBackend backend;
  TranscriptionService service(models, decoder, backend);
  OptionsMessage options;
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");

  const std::vector<FakeBackend::Word> invalid_records{
      {"", 10, 20, 0.5F},
      {"zero", 20, 20, 0.5F},
      {"negative", -1, 20, 0.5F},
      {"outside", 90, 101, 0.5F},
      {"nan", 10, 20, std::numeric_limits<float>::quiet_NaN()},
  };
  for (const auto& invalid : invalid_records) {
    backend.words = {invalid};
    const auto result = service.transcribe(std::filesystem::absolute(input), options);
    ASSERT_FALSE(result) << invalid.text;
    EXPECT_EQ(result.error().code, ErrorCode::BackendFailed) << invalid.text;
  }

  backend.words = {{"first", 10, 20, 0.5F}, {"overlap", 19, 30, 0.5F}};
  const auto overlap = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(overlap);
  EXPECT_EQ(overlap.error().code, ErrorCode::BackendFailed);

  backend.words = {{"later", 20, 30, 0.5F}, {"earlier", 10, 20, 0.5F}};
  const auto unsorted = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(unsorted);
  EXPECT_EQ(unsorted.error().code, ErrorCode::BackendFailed);

  backend.words = {{"first", 10, 20, 0.5F}, {"adjacent", 20, 30, 0.5F}};
  const auto adjacent = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_TRUE(adjacent) << adjacent.error().message;
  ASSERT_EQ(adjacent.value().words_size(), 2);
  EXPECT_EQ(adjacent.value().words(1).start_centiseconds(), 20);
}

TEST(TranscriptionService, RejectsMalformedBackendText) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "audio.wav";
  std::ofstream(input) << "fixture";
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  ModelManager models(directory.path() / "models", fetcher, small_model(fetcher.bytes));
  FakeDecoder decoder;
  FakeBackend backend;
  TranscriptionService service(models, decoder, backend);
  OptionsMessage options;
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");

  std::string malformed_utf8;
  malformed_utf8.append("\xC0\xAF", 2);
  backend.words = {{malformed_utf8, 10, 20, 0.5F}};
  auto result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.words = {{std::string("embedded\0nul", 12), 10, 20, 0.5F}};
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.words = {{std::string(64U * 1024U + 1U, 'x'), 10, 20, 0.5F}};
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.words = {{"valid", 10, 20, 0.5F}};
  std::string invalid_language;
  invalid_language.append("\xC0\xAF", 2);
  backend.detected_language = invalid_language;
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.detected_language = std::string("en\0invalid", 10);
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.detected_language = std::string(257U, 'x');
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.detected_language.clear();
  backend.output_backend = invalid_language;
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.output_backend = std::string("fake\0backend", 12);
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.output_backend = std::string(257U, 'x');
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);

  backend.output_backend.clear();
  backend.capability_backend = std::string("fake\0backend", 12);
  result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::BackendFailed);
}

TEST(TranscriptionService, CeilsPartialCentisecondAudioDurationForWordValidation) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "audio.wav";
  std::ofstream(input) << "fixture";
  FakeFetcher fetcher;
  fetcher.bytes = "test-model";
  ModelManager models(directory.path() / "models", fetcher, small_model(fetcher.bytes));
  FakeDecoder decoder;
  decoder.sample_count = 16'001U;
  FakeBackend backend;
  backend.words = {{"partial", 100, 101, 0.5F}};
  TranscriptionService service(models, decoder, backend);
  OptionsMessage options;
  options.set_schema_version(kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");

  const auto result = service.transcribe(std::filesystem::absolute(input), options);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result.value().duration_centiseconds(), 101);
  ASSERT_EQ(result.value().words_size(), 1);
  EXPECT_EQ(result.value().words(0).end_centiseconds(), 101);
}

TEST(FfmpegDecoder, DecodesOnlyExactSourceWindowAtSixteenKilohertz) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "window.wav";
  write_pcm16_wav(input, 160'000U);
  auto decoder = make_ffmpeg_audio_decoder();
  ASSERT_NE(decoder, nullptr);
  std::size_t progress_calls = 0;
  const auto result =
      decoder->decode(std::filesystem::absolute(input),
                      AudioRange{.start_centiseconds = 50, .duration_centiseconds = 10}, {},
                      [&progress_calls](double, std::string_view) { ++progress_calls; });
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result.value().sample_rate, 16'000U);
  EXPECT_EQ(result.value().samples.size(), 1'600U);
  ASSERT_FALSE(result.value().samples.empty());
  EXPECT_NEAR(result.value().samples.front(), 0.0, 0.001);
  // The selected 100 ms window is reached in the first packet; reading to
  // EOF would emit periodic decode progress for this 10-second fixture.
  EXPECT_LE(progress_calls, 1U);
}

TEST(FfmpegDecoder, KeepsContinuousTimelineWhenResamplingFortyEightKilohertz) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "window-48k.wav";
  write_pcm16_wav(input, 480'000U, 48'000U);
  auto decoder = make_ffmpeg_audio_decoder();
  ASSERT_NE(decoder, nullptr);
  const auto result =
      decoder->decode(std::filesystem::absolute(input),
                      AudioRange{.start_centiseconds = 7, .duration_centiseconds = 13}, {}, {});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result.value().sample_rate, 16'000U);
  ASSERT_EQ(result.value().samples.size(), 2'080U);
  EXPECT_NEAR(result.value().samples.front(), 0.0, 0.002);
}

TEST(FfmpegDecoder, KeepsContinuousTimelineWhenResamplingFortyFourPointOneKilohertz) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "window-44k.wav";
  write_pcm16_wav(input, 441'000U, 44'100U);
  auto decoder = make_ffmpeg_audio_decoder();
  ASSERT_NE(decoder, nullptr);
  const auto result =
      decoder->decode(std::filesystem::absolute(input),
                      AudioRange{.start_centiseconds = 11, .duration_centiseconds = 17}, {}, {});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result.value().sample_rate, 16'000U);
  ASSERT_EQ(result.value().samples.size(), 2'720U);
  EXPECT_NEAR(result.value().samples.front(), 0.0, 0.002);
}

TEST(FfmpegDecoder, FlushesResamplerForBoundedWindowEndingAtEndOfFile) {
  TemporaryDirectory directory;
  const auto input = directory.path() / "window-44k-tail.wav";
  write_pcm16_wav(input, 44'100U, 44'100U);
  auto decoder = make_ffmpeg_audio_decoder();
  ASSERT_NE(decoder, nullptr);
  const auto result =
      decoder->decode(std::filesystem::absolute(input),
                      AudioRange{.start_centiseconds = 83, .duration_centiseconds = 17}, {}, {});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result.value().sample_rate, 16'000U);
  EXPECT_EQ(result.value().samples.size(), 2'720U);
}

} // namespace
} // namespace video_editor::transcription

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/transcription_service/transcription_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace video_editor::transcription {
namespace {

// Keep backend text bounded before it reaches protobuf consumers and the UI.
constexpr std::size_t kMaximumWordTextBytes = 64U * 1024U;
constexpr std::size_t kMaximumMetadataTextBytes = 256U;

[[nodiscard]] Error failure(const ErrorCode code, std::string message) {
  return {.code = code, .native_code = 0, .message = std::move(message), .retryable = false};
}

[[nodiscard]] bool valid_language(std::string_view language) noexcept {
  if (language == "auto") {
    return true;
  }
  if (language.size() != 2U) {
    return false;
  }
  return std::islower(static_cast<unsigned char>(language[0])) != 0 &&
         std::islower(static_cast<unsigned char>(language[1])) != 0;
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<std::uint8_t>(static_cast<unsigned char>(value[index]));
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
      codepoint = static_cast<std::uint32_t>(lead & 0x1FU);
      minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      codepoint = static_cast<std::uint32_t>(lead & 0x0FU);
      minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      codepoint = static_cast<std::uint32_t>(lead & 0x07U);
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > value.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto byte =
          static_cast<std::uint8_t>(static_cast<unsigned char>(value[index + offset]));
      if ((byte & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

[[nodiscard]] bool valid_bounded_text(const std::string_view value,
                                      const std::size_t maximum_bytes) noexcept {
  return value.size() <= maximum_bytes && value.find('\0') == std::string_view::npos &&
         valid_utf8(value);
}

} // namespace

bool validate_options(const OptionsMessage& options, std::string& diagnostic) noexcept {
  if (options.schema_version() != kTranscriptionSchemaVersion) {
    diagnostic = "schema_version must be 2";
    return false;
  }
  if (options.model_id() != kWhisperModelId) {
    diagnostic = "model_id must be base";
    return false;
  }
  if (!valid_language(options.language())) {
    diagnostic = "language must be auto or a lowercase two-letter language code";
    return false;
  }
  if (options.thread_count() > 256U) {
    diagnostic = "thread_count must be zero or no greater than 256";
    return false;
  }
  if (options.source_start_centiseconds() < 0 || options.source_duration_centiseconds() < 0) {
    diagnostic = "source transcription range cannot be negative";
    return false;
  }
  if (options.source_duration_centiseconds() == 0 && options.source_start_centiseconds() != 0) {
    diagnostic = "source_duration_centiseconds must be positive when a source start is set";
    return false;
  }
  if (options.source_start_centiseconds() >
      std::numeric_limits<std::int64_t>::max() - options.source_duration_centiseconds()) {
    diagnostic = "source transcription range overflows the supported time domain";
    return false;
  }
  return true;
}

TranscriptionService::TranscriptionService(ModelManager& models, AudioDecoder& decoder,
                                           TranscriptionBackend& backend)
    : models_(models), decoder_(decoder), backend_(backend) {}

BackendCapabilities TranscriptionService::capabilities() const {
  return backend_.capabilities();
}

Result<ResultMessage> TranscriptionService::transcribe(const std::filesystem::path& input,
                                                       const OptionsMessage& options,
                                                       const std::stop_token cancellation,
                                                       const ProgressCallback& progress) const {
  std::string diagnostic;
  if (!validate_options(options, diagnostic)) {
    return Result<ResultMessage>::failure(
        failure(ErrorCode::InvalidOptions, std::move(diagnostic)));
  }
  std::error_code filesystem_error;
  if (!input.is_absolute()) {
    return Result<ResultMessage>::failure(
        failure(ErrorCode::InvalidInput, "transcription input must be an absolute path"));
  }
  if (!std::filesystem::is_regular_file(input, filesystem_error) || filesystem_error) {
    return Result<ResultMessage>::failure(
        failure(ErrorCode::InputNotFound, "transcription input is not a regular file"));
  }
  if (cancellation.stop_requested()) {
    return Result<ResultMessage>::failure(failure(ErrorCode::Cancelled, "transcription cancelled"));
  }
  if (progress)
    progress(0.2, "model");
  const auto model = models_.ensure(cancellation);
  if (!model) {
    return Result<ResultMessage>::failure(model.error());
  }
  if (progress)
    progress(0.3, "decoding");
  const AudioRange range{.start_centiseconds = options.source_start_centiseconds(),
                         .duration_centiseconds = options.source_duration_centiseconds()};
  const auto audio = decoder_.decode(input, range, cancellation,
                                     [&](const double value, const std::string_view phase) {
                                       if (progress)
                                         progress(0.3 + std::clamp(value, 0.0, 1.0) * 0.2, phase);
                                     });
  if (!audio) {
    return Result<ResultMessage>::failure(audio.error());
  }
  if (audio.value().sample_rate != 16'000U || audio.value().samples.empty()) {
    return Result<ResultMessage>::failure(
        failure(ErrorCode::AudioDecodeFailed, "audio decoder did not produce 16 kHz mono audio"));
  }
  if (cancellation.stop_requested()) {
    return Result<ResultMessage>::failure(failure(ErrorCode::Cancelled, "transcription cancelled"));
  }
  if (progress)
    progress(0.5, "transcribing");
  auto result = backend_.transcribe(model.value(), audio.value(), options, cancellation,
                                    [&](const double value, const std::string_view phase) {
                                      if (progress) {
                                        progress(0.5 + std::clamp(value, 0.0, 1.0) * 0.45, phase);
                                      }
                                    });
  if (!result) {
    return Result<ResultMessage>::failure(result.error());
  }
  ResultMessage output = std::move(result).value();
  const auto capabilities = backend_.capabilities();
  if (!valid_bounded_text(output.detected_language(), kMaximumMetadataTextBytes) ||
      !valid_bounded_text(output.backend(), kMaximumMetadataTextBytes) ||
      !valid_bounded_text(capabilities.backend, kMaximumMetadataTextBytes)) {
    return Result<ResultMessage>::failure(
        failure(ErrorCode::BackendFailed, "transcription backend returned invalid metadata text"));
  }
  const auto sample_count = static_cast<std::uint64_t>(audio.value().samples.size());
  const auto duration_centiseconds = sample_count / 160U + (sample_count % 160U == 0U ? 0U : 1U);
  const auto decoded_duration = static_cast<std::int64_t>(std::min<std::uint64_t>(
      duration_centiseconds, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
  std::int64_t previous_end = 0;
  bool have_previous_word = false;
  for (const auto& word : output.words()) {
    if (!valid_bounded_text(word.text(), kMaximumWordTextBytes) || word.text().empty() ||
        word.start_centiseconds() < 0 || word.start_centiseconds() >= word.end_centiseconds() ||
        word.end_centiseconds() > decoded_duration || !std::isfinite(word.probability()) ||
        word.probability() < 0.0F || word.probability() > 1.0F ||
        (have_previous_word && word.start_centiseconds() < previous_end)) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::BackendFailed, "transcription backend returned invalid word records"));
    }
    previous_end = word.end_centiseconds();
    have_previous_word = true;
  }
  output.set_schema_version(kTranscriptionSchemaVersion);
  output.set_model_id(options.model_id());
  output.set_model_digest_algorithm(kWhisperModelDigestAlgorithm);
  output.set_model_digest(kWhisperModelDigest);
  output.set_source_start_centiseconds(range.start_centiseconds);
  output.set_source_duration_centiseconds(range.duration_centiseconds);
  if (range.start_centiseconds != 0) {
    for (auto& word : *output.mutable_words()) {
      if (word.start_centiseconds() >
              std::numeric_limits<std::int64_t>::max() - range.start_centiseconds ||
          word.end_centiseconds() >
              std::numeric_limits<std::int64_t>::max() - range.start_centiseconds) {
        return Result<ResultMessage>::failure(
            failure(ErrorCode::BackendFailed, "transcription timestamp overflow"));
      }
      word.set_start_centiseconds(word.start_centiseconds() + range.start_centiseconds);
      word.set_end_centiseconds(word.end_centiseconds() + range.start_centiseconds);
    }
  }
  output.set_duration_centiseconds(decoded_duration);
  output.set_vulkan_available(capabilities.vulkan_available);
  // The pinned whisper.cpp C API cannot report which ggml backend actually
  // executed. Keep this field false rather than claiming runtime Vulkan use.
  output.set_vulkan_used(false);
  if (output.backend().empty())
    output.set_backend(capabilities.backend);
  if (progress)
    progress(1.0, "complete");
  return Result<ResultMessage>::success(std::move(output));
}

} // namespace video_editor::transcription

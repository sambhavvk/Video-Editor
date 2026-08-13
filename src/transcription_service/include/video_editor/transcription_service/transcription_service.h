// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/result.h"
#include "video_editor/job_service.pb.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::transcription {

inline constexpr std::uint32_t kTranscriptionSchemaVersion = 1;
inline constexpr std::string_view kWhisperModelId{"base"};
inline constexpr std::string_view kWhisperModelFilename{"ggml-base.bin"};
inline constexpr std::string_view kWhisperModelUrl{
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"};
inline constexpr std::string_view kWhisperModelDigestAlgorithm{"sha1"};
inline constexpr std::string_view kWhisperModelDigest{
    "465707469ff3a37a2b9b8d8f89f2f99de7299dac"};
inline constexpr std::uintmax_t kWhisperModelBytes{147951465U};

using OptionsMessage = jobs::v1::TranscribeOptions;
using ResultMessage = jobs::v1::TranscriptionResult;
using ProgressCallback = std::function<void(double, std::string_view)>;

enum class ErrorCode : std::uint8_t {
  InvalidOptions,
  InvalidInput,
  InputNotFound,
  Cancelled,
  AudioDecodeFailed,
  ModelUnavailable,
  ModelDownloadFailed,
  ModelSizeMismatch,
  ModelChecksumMismatch,
  BackendUnavailable,
  BackendFailed,
};

struct Error final {
  ErrorCode code{ErrorCode::BackendFailed};
  int native_code{0};
  std::string message;
  bool retryable{false};
};

template <typename T> using Result = edit::Result<T, Error>;

struct ModelDescriptor final {
  std::string id;
  std::string filename;
  std::string url;
  std::string digest_algorithm;
  std::string digest;
  std::uintmax_t expected_bytes{0};
};

[[nodiscard]] const ModelDescriptor& default_model_descriptor() noexcept;

class ModelFetcher {
public:
  virtual ~ModelFetcher() = default;
  [[nodiscard]] virtual Result<std::uintmax_t>
  fetch(const ModelDescriptor& model, const std::filesystem::path& destination,
        std::stop_token cancellation) = 0;
};

class ModelManager final {
public:
  ModelManager(std::filesystem::path cache_directory, ModelFetcher& fetcher,
               ModelDescriptor model = default_model_descriptor());

  [[nodiscard]] Result<std::filesystem::path>
  ensure(std::stop_token cancellation = {}) const;

private:
  std::filesystem::path cache_directory_;
  ModelFetcher& fetcher_;
  ModelDescriptor model_;
};

struct AudioData final {
  std::vector<float> samples;
  std::uint32_t sample_rate{16'000};
};

class AudioDecoder {
public:
  virtual ~AudioDecoder() = default;
  [[nodiscard]] virtual Result<AudioData>
  decode(const std::filesystem::path& input, std::stop_token cancellation,
         const ProgressCallback& progress) = 0;
};

struct BackendCapabilities final {
  bool available{false};
  bool vulkan_available{false};
  std::string backend;
};

class TranscriptionBackend {
public:
  virtual ~TranscriptionBackend() = default;
  [[nodiscard]] virtual BackendCapabilities capabilities() const = 0;
  [[nodiscard]] virtual Result<ResultMessage>
  transcribe(const std::filesystem::path& model, const AudioData& audio,
             const OptionsMessage& options, std::stop_token cancellation,
             const ProgressCallback& progress) = 0;
};

class TranscriptionService final {
public:
  TranscriptionService(ModelManager& models, AudioDecoder& decoder,
                       TranscriptionBackend& backend);

  [[nodiscard]] Result<ResultMessage>
  transcribe(const std::filesystem::path& input, const OptionsMessage& options,
             std::stop_token cancellation = {},
             const ProgressCallback& progress = {}) const;

private:
  ModelManager& models_;
  AudioDecoder& decoder_;
  TranscriptionBackend& backend_;
};

[[nodiscard]] bool validate_options(const OptionsMessage& options,
                                    std::string& diagnostic) noexcept;

[[nodiscard]] std::unique_ptr<AudioDecoder> make_ffmpeg_audio_decoder();
[[nodiscard]] std::unique_ptr<TranscriptionBackend> make_default_backend();
[[nodiscard]] std::unique_ptr<ModelFetcher> make_unavailable_model_fetcher();

} // namespace video_editor::transcription

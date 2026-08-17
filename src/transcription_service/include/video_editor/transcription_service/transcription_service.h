// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/result.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/transcription_service/model_manifest.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::transcription {

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
  [[nodiscard]] virtual Result<std::uintmax_t> fetch(const ModelDescriptor& model,
                                                     const std::filesystem::path& destination,
                                                     std::stop_token cancellation) = 0;
};

class ModelManager final {
public:
  ModelManager(std::filesystem::path cache_directory, ModelFetcher& fetcher,
               ModelDescriptor model = default_model_descriptor());

  [[nodiscard]] Result<std::filesystem::path> ensure(std::stop_token cancellation = {}) const;
  // Checks the staged cache artifact only; it never invokes ModelFetcher.
  [[nodiscard]] bool verify(std::stop_token cancellation = {}) const noexcept;
  [[nodiscard]] std::filesystem::path model_path() const;

private:
  std::filesystem::path cache_directory_;
  ModelFetcher& fetcher_;
  ModelDescriptor model_;
};

struct AudioData final {
  std::vector<float> samples;
  std::uint32_t sample_rate{16'000};
};

// A source-relative window in the asset's media timeline. The all-zero value
// means the complete input. AudioData samples always begin at zero for this
// window; TranscriptionService translates returned word timestamps back to
// source-absolute centiseconds.
struct AudioRange final {
  std::int64_t start_centiseconds{0};
  std::int64_t duration_centiseconds{0};

  [[nodiscard]] bool is_full_input() const noexcept {
    return start_centiseconds == 0 && duration_centiseconds == 0;
  }
};

class AudioDecoder {
public:
  virtual ~AudioDecoder() = default;
  [[nodiscard]] virtual Result<AudioData> decode(const std::filesystem::path& input,
                                                 const AudioRange& range,
                                                 std::stop_token cancellation,
                                                 const ProgressCallback& progress) = 0;
};

struct BackendCapabilities final {
  bool available{false};
  // This is a compiled/provenance assertion supplied by the build, not a
  // runtime device check. whisper.cpp 1.9.2 does not expose the selected
  // ggml backend through its public C API.
  bool vulkan_available{false};
  std::string backend;
};

class TranscriptionBackend {
public:
  virtual ~TranscriptionBackend() = default;
  [[nodiscard]] virtual BackendCapabilities capabilities() const = 0;
  [[nodiscard]] virtual Result<ResultMessage> transcribe(const std::filesystem::path& model,
                                                         const AudioData& audio,
                                                         const OptionsMessage& options,
                                                         std::stop_token cancellation,
                                                         const ProgressCallback& progress) = 0;
};

class TranscriptionService final {
public:
  TranscriptionService(ModelManager& models, AudioDecoder& decoder, TranscriptionBackend& backend);

  [[nodiscard]] Result<ResultMessage> transcribe(const std::filesystem::path& input,
                                                 const OptionsMessage& options,
                                                 std::stop_token cancellation = {},
                                                 const ProgressCallback& progress = {}) const;
  [[nodiscard]] BackendCapabilities capabilities() const;

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

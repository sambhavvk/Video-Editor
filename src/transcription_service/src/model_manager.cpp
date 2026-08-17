// SPDX-License-Identifier: MPL-2.0
#include "video_editor/transcription_service/transcription_service.h"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace video_editor::transcription {
namespace {

[[nodiscard]] Error error(const ErrorCode code, std::string message, const bool retryable = false) {
  return {.code = code, .native_code = 0, .message = std::move(message), .retryable = retryable};
}

[[nodiscard]] std::optional<std::string> digest_file(const std::filesystem::path& path,
                                                     const std::stop_token cancellation) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
  if (raw_context == nullptr) {
    return std::nullopt;
  }
  const auto cleanup = [](EVP_MD_CTX* context) { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(raw_context, EVP_sha1(), nullptr) != 1) {
    cleanup(raw_context);
    return std::nullopt;
  }
  std::array<char, 64U * 1024U> buffer{};
  while (input.good()) {
    if (cancellation.stop_requested()) {
      cleanup(raw_context);
      return std::nullopt;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0 &&
        EVP_DigestUpdate(raw_context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
      cleanup(raw_context);
      return std::nullopt;
    }
  }
  if (!input.eof()) {
    cleanup(raw_context);
    return std::nullopt;
  }
  if (cancellation.stop_requested()) {
    cleanup(raw_context);
    return std::nullopt;
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  const bool finalized = EVP_DigestFinal_ex(raw_context, digest.data(), &length) == 1;
  cleanup(raw_context);
  if (!finalized) {
    return std::nullopt;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index) {
    output << std::setw(2) << static_cast<unsigned>(digest[index]);
  }
  return output.str();
}

[[nodiscard]] bool matches(const std::filesystem::path& path, const ModelDescriptor& model,
                           const std::stop_token cancellation) {
  if (model.digest_algorithm != "sha1" || model.digest.size() != 40U ||
      model.expected_bytes == 0U) {
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error) || filesystem_error) {
    return false;
  }
  if (std::filesystem::file_size(path, filesystem_error) != model.expected_bytes ||
      filesystem_error) {
    return false;
  }
  const auto digest = digest_file(path, cancellation);
  return digest.has_value() && *digest == model.digest;
}

} // namespace

const ModelDescriptor& default_model_descriptor() noexcept {
  static const ModelDescriptor descriptor{
      .id = std::string(kWhisperModelId),
      .filename = std::string(kWhisperModelFilename),
      .url = std::string(kWhisperModelUrl),
      .digest_algorithm = std::string(kWhisperModelDigestAlgorithm),
      .digest = std::string(kWhisperModelDigest),
      .expected_bytes = kWhisperModelBytes,
  };
  return descriptor;
}

ModelManager::ModelManager(std::filesystem::path cache_directory, ModelFetcher& fetcher,
                           ModelDescriptor model)
    : cache_directory_(std::move(cache_directory)), fetcher_(fetcher), model_(std::move(model)) {}

bool ModelManager::verify(const std::stop_token cancellation) const noexcept {
  try {
    return !cancellation.stop_requested() &&
           matches(cache_directory_ / model_.filename, model_, cancellation);
  } catch (...) {
    return false;
  }
}

std::filesystem::path ModelManager::model_path() const {
  return cache_directory_ / model_.filename;
}

Result<std::filesystem::path> ModelManager::ensure(const std::stop_token cancellation) const {
  if (model_.digest_algorithm != "sha1" || model_.digest.size() != 40U ||
      model_.expected_bytes == 0U || model_.filename.empty()) {
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::ModelUnavailable, "model manifest is invalid"));
  }
  if (cancellation.stop_requested()) {
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::Cancelled, "model acquisition was cancelled"));
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(cache_directory_, filesystem_error);
  if (filesystem_error) {
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::ModelUnavailable, "cannot create the model cache directory"));
  }
  const auto destination = cache_directory_ / model_.filename;
  if (verify(cancellation)) {
    return Result<std::filesystem::path>::success(destination);
  }

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary = destination.string() + ".part." + std::to_string(stamp);
  const auto fetched = fetcher_.fetch(model_, temporary, cancellation);
  if (!fetched) {
    std::filesystem::remove(temporary, filesystem_error);
    return Result<std::filesystem::path>::failure(fetched.error());
  }
  if (cancellation.stop_requested()) {
    std::filesystem::remove(temporary, filesystem_error);
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::Cancelled, "model acquisition was cancelled"));
  }
  if (!matches(temporary, model_, cancellation)) {
    if (cancellation.stop_requested()) {
      std::filesystem::remove(temporary, filesystem_error);
      return Result<std::filesystem::path>::failure(
          error(ErrorCode::Cancelled, "model acquisition was cancelled"));
    }
    std::error_code inspect_error;
    const auto size = std::filesystem::file_size(temporary, inspect_error);
    std::filesystem::remove(temporary, filesystem_error);
    if (!inspect_error && size != model_.expected_bytes) {
      return Result<std::filesystem::path>::failure(
          error(ErrorCode::ModelSizeMismatch, "downloaded model has an unexpected byte size"));
    }
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::ModelChecksumMismatch, "downloaded model failed SHA-1 verification"));
  }
  const bool destination_exists =
      std::filesystem::exists(destination, filesystem_error) && !filesystem_error;
  const auto backup = destination.string() + ".invalid." + std::to_string(stamp);
  bool backed_up = false;
  if (destination_exists) {
    // Keep the old artifact recoverable until the verified staging file has
    // been installed. This also works on Windows, where rename() cannot
    // replace an existing destination.
    std::filesystem::rename(destination, backup, filesystem_error);
    if (filesystem_error) {
      std::filesystem::remove(temporary, filesystem_error);
      return Result<std::filesystem::path>::failure(
          error(ErrorCode::ModelUnavailable, "cannot replace the invalid cached model"));
    }
    backed_up = true;
  }
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (backed_up) {
      std::error_code restore_error;
      std::filesystem::rename(backup, destination, restore_error);
    }
    return Result<std::filesystem::path>::failure(
        error(ErrorCode::ModelUnavailable, "cannot atomically install the model"));
  }
  if (backed_up) {
    std::filesystem::remove(backup, filesystem_error);
  }
  return Result<std::filesystem::path>::success(destination);
}

namespace {
class UnavailableFetcher final : public ModelFetcher {
public:
  [[nodiscard]] Result<std::uintmax_t> fetch(const ModelDescriptor&, const std::filesystem::path&,
                                             std::stop_token) override {
    return Result<std::uintmax_t>::failure(error(
        ErrorCode::ModelDownloadFailed, "no model downloader is configured for this worker", true));
  }
};
} // namespace

std::unique_ptr<ModelFetcher> make_unavailable_model_fetcher() {
  return std::make_unique<UnavailableFetcher>();
}

} // namespace video_editor::transcription

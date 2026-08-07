// SPDX-License-Identifier: MPL-2.0
#include "video_editor/asset_service/fingerprint.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>

namespace video_editor::assets {
namespace {

constexpr std::size_t kRegionBytes = 1024U * 1024U;
constexpr std::size_t kIoBlockBytes = 64U * 1024U;

struct DigestContextDeleter {
  void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

using DigestContext = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

media::MediaError error(media::MediaErrorCode code, std::string message) {
  return {.code = code, .native_code = 0, .message = std::move(message)};
}

bool update_stream(EVP_MD_CTX* context, std::ifstream& input, std::uintmax_t bytes) {
  std::array<char, kIoBlockBytes> buffer{};
  while (bytes > 0U && input.good()) {
    const auto requested = static_cast<std::streamsize>(std::min<std::uintmax_t>(bytes, buffer.size()));
    input.read(buffer.data(), requested);
    const std::streamsize received = input.gcount();
    if (received <= 0) {
      return false;
    }
    if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(received)) != 1) {
      return false;
    }
    bytes -= static_cast<std::uintmax_t>(received);
  }
  return bytes == 0U;
}

std::optional<std::string> finish_digest(EVP_MD_CTX* context) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &length) != 1) {
    return std::nullopt;
  }
  std::ostringstream value;
  value << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index) {
    value << std::setw(2) << static_cast<unsigned>(digest[index]);
  }
  return value.str();
}

std::optional<std::string> hash_full(const std::filesystem::path& path, const std::uintmax_t size) {
  std::ifstream input(path, std::ios::binary);
  DigestContext context(EVP_MD_CTX_new());
  if (!input || context == nullptr || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      !update_stream(context.get(), input, size)) {
    return std::nullopt;
  }
  return finish_digest(context.get());
}

std::optional<std::string> hash_quick(const std::filesystem::path& path,
                                      const std::uintmax_t size) {
  std::ifstream input(path, std::ios::binary);
  DigestContext context(EVP_MD_CTX_new());
  if (!input || context == nullptr || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    return std::nullopt;
  }

  if (EVP_DigestUpdate(context.get(), &size, sizeof(size)) != 1) {
    return std::nullopt;
  }

  const std::uintmax_t first_region = std::min<std::uintmax_t>(size, kRegionBytes);
  if (!update_stream(context.get(), input, first_region)) {
    return std::nullopt;
  }
  if (size > first_region) {
    const std::uintmax_t last_region = std::min<std::uintmax_t>(size - first_region, kRegionBytes);
    input.clear();
    input.seekg(static_cast<std::streamoff>(size - last_region), std::ios::beg);
    if (!input || !update_stream(context.get(), input, last_region)) {
      return std::nullopt;
    }
  }
  return finish_digest(context.get());
}

} // namespace

media::Result<FileFingerprint> fingerprint_file(const std::filesystem::path& path,
                                                const bool include_full_hash) {
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error)) {
    return media::Result<FileFingerprint>::failure(
        error(media::MediaErrorCode::FileNotFound, "cannot fingerprint a missing media file"));
  }

  const std::uintmax_t size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    return media::Result<FileFingerprint>::failure(
        error(media::MediaErrorCode::OpenFailed, "cannot read media file size"));
  }
  const auto modified_time = std::filesystem::last_write_time(path, filesystem_error);
  if (filesystem_error) {
    return media::Result<FileFingerprint>::failure(
        error(media::MediaErrorCode::OpenFailed, "cannot read media modification time"));
  }
  const auto modified = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            modified_time.time_since_epoch())
                            .count();

  const std::optional<std::string> quick = hash_quick(path, size);
  if (!quick.has_value()) {
    return media::Result<FileFingerprint>::failure(
        error(media::MediaErrorCode::OpenFailed, "cannot hash media file"));
  }

  FileFingerprint fingerprint{
      .size = size,
      .modified_nanoseconds = modified,
      .quick_sha256 = *quick,
      .full_sha256 = std::nullopt,
  };
  if (include_full_hash) {
    fingerprint.full_sha256 = hash_full(path, size);
    if (!fingerprint.full_sha256.has_value()) {
      return media::Result<FileFingerprint>::failure(
          error(media::MediaErrorCode::OpenFailed, "cannot compute complete media hash"));
    }
  }
  return media::Result<FileFingerprint>::success(std::move(fingerprint));
}

} // namespace video_editor::assets

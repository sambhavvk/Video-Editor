// SPDX-License-Identifier: MPL-2.0
#include "video_editor/asset_service/asset_service.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace video_editor::assets {
namespace {

bool is_long_gop_creator_codec(const std::string& codec) {
  return codec == "h264" || codec == "hevc" || codec == "av1" || codec == "vp9";
}

media::MediaError changed_content_error() {
  return {
      .code = media::MediaErrorCode::InvalidArgument,
      .native_code = 0,
      .message = "replacement media does not match the original fingerprint",
  };
}

[[nodiscard]] bool is_still_image_extension(std::string extension) {
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
         extension == ".bmp" || extension == ".webp" || extension == ".tif" ||
         extension == ".tiff" || extension == ".ppm";
}

struct ImageSequencePattern final {
  std::filesystem::path first_file;
  std::filesystem::path pattern_uri;
  int frame_count{0};
};

[[nodiscard]] std::optional<ImageSequencePattern>
detect_image_sequence(const std::filesystem::path& uri) {
  std::error_code error;
  std::filesystem::path file = uri;
  if (std::filesystem::is_directory(uri, error)) {
    std::vector<std::filesystem::path> images;
    for (const auto& entry : std::filesystem::directory_iterator(uri, error)) {
      if (entry.is_regular_file() && is_still_image_extension(entry.path().extension().string())) {
        images.push_back(entry.path());
      }
    }
    std::sort(images.begin(), images.end());
    if (images.empty()) {
      return std::nullopt;
    }
    file = images.front();
  } else if (!std::filesystem::is_regular_file(uri, error) ||
             !is_still_image_extension(uri.extension().string())) {
    return std::nullopt;
  }

  const std::string stem = file.stem().string();
  std::size_t digit_start = stem.size();
  while (digit_start > 0 && std::isdigit(static_cast<unsigned char>(stem[digit_start - 1]))) {
    --digit_start;
  }
  const std::size_t digit_count = stem.size() - digit_start;
  if (digit_count == 0) {
    return std::nullopt;
  }
  const std::string prefix = stem.substr(0, digit_start);
  const std::string extension = file.extension().string();
  int first_index = 0;
  try {
    first_index = std::stoi(stem.substr(digit_start));
  } catch (...) {
    return std::nullopt;
  }

  int count = 0;
  for (int index = first_index;; ++index) {
    std::ostringstream name;
    name << prefix << std::setw(static_cast<int>(digit_count)) << std::setfill('0') << index
         << extension;
    const auto candidate = file.parent_path() / name.str();
    if (!std::filesystem::is_regular_file(candidate, error)) {
      break;
    }
    ++count;
  }
  if (count < 2) {
    return std::nullopt;
  }

  std::ostringstream pattern;
  pattern << prefix << "%0" << digit_count << "d" << extension;
  return ImageSequencePattern{.first_file = file,
                              .pattern_uri = file.parent_path() / pattern.str(),
                              .frame_count = count};
}

} // namespace

media::Result<AssetRecord> AssetService::import(const std::filesystem::path& uri,
                                                 const ImportOptions& options) {
  const auto sequence = detect_image_sequence(uri);
  const std::filesystem::path fingerprint_uri =
      sequence.has_value() ? sequence->first_file : uri;
  const std::filesystem::path probe_uri = sequence.has_value() ? sequence->pattern_uri : uri;
  auto fingerprint_result = fingerprint_file(fingerprint_uri, options.compute_full_hash);
  if (!fingerprint_result) {
    return media::Result<AssetRecord>::failure(fingerprint_result.error());
  }
  FileFingerprint fingerprint = std::move(fingerprint_result).value();
  if (sequence.has_value()) {
    const std::string material =
        fingerprint.quick_sha256 + "\n" + sequence->pattern_uri.filename().string();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_length = 0;
    EVP_Digest(material.data(), material.size(), digest.data(), &digest_length, EVP_sha256(),
               nullptr);
    std::ostringstream mixed;
    mixed << std::hex << std::setfill('0');
    for (unsigned index = 0; index < digest_length; ++index) {
      mixed << std::setw(2) << static_cast<unsigned>(digest[index]);
    }
    fingerprint.quick_sha256 = mixed.str();
  }
  auto descriptor = media::probe(probe_uri, options.probe);
  if (!descriptor) {
    return media::Result<AssetRecord>::failure(descriptor.error());
  }

  AssetRecord asset{
      .id = make_asset_id(fingerprint, probe_uri),
      .uri = std::filesystem::absolute(probe_uri),
      .fingerprint = std::move(fingerprint),
      .descriptor = std::move(descriptor).value(),
      .availability = AssetAvailability::Online,
      .proxy = std::nullopt,
  };
  if (sequence.has_value()) {
    asset.descriptor.format_name = "image2-sequence";
    if ((!asset.descriptor.duration_microseconds.has_value() ||
         *asset.descriptor.duration_microseconds <= 0) &&
        sequence->frame_count > 0) {
      asset.descriptor.duration_microseconds =
          static_cast<std::int64_t>(sequence->frame_count) * 1'000'000 / 30;
    }
  }
  return media::Result<AssetRecord>::success(std::move(asset));
}

media::Result<AssetRecord> AssetService::relink(const AssetRecord& existing,
                                                 const std::filesystem::path& replacement,
                                                 const bool permit_changed_content) {
  auto candidate = import(
      replacement,
      {.compute_full_hash = existing.fingerprint.full_sha256.has_value(), .probe = {}});
  if (!candidate) {
    return candidate;
  }

  AssetRecord result = std::move(candidate).value();
  const bool matches = existing.fingerprint.content_matches(result.fingerprint) &&
                       (!existing.fingerprint.full_sha256.has_value() ||
                        existing.fingerprint.full_sha256 == result.fingerprint.full_sha256);
  if (!matches && !permit_changed_content) {
    return media::Result<AssetRecord>::failure(changed_content_error());
  }
  result.id = existing.id;
  result.availability = matches ? AssetAvailability::Online : AssetAvailability::Changed;
  result.proxy.reset();
  return media::Result<AssetRecord>::success(std::move(result));
}

bool AssetService::should_recommend_proxy(const AssetRecord& asset) noexcept {
  for (const auto& stream : asset.descriptor.streams) {
    if (!stream.video.has_value()) {
      continue;
    }
    if (stream.video->frame_count > 0 && stream.video->frame_count <= 1) {
      continue;
    }
    const std::string codec = stream.codec_name;
    if (codec == "png" || codec == "mjpeg" || codec == "bmp" || codec == "ppm" ||
        codec == "webp" || codec == "tiff" || codec == "gif") {
      continue;
    }
    const bool is_4k = stream.video->width >= 3'840 || stream.video->height >= 2'160;
    if (is_4k && is_long_gop_creator_codec(codec)) {
      return true;
    }
  }
  return false;
}

ProxyProfile AssetService::default_proxy_profile(const AssetRecord& asset) noexcept {
  ProxyProfile profile;
  for (const auto& stream : asset.descriptor.streams) {
    if (stream.video.has_value() &&
        (stream.video->width > 3'840 || stream.video->height > 2'160)) {
      profile.maximum_width = 1'280;
      profile.maximum_height = 720;
      break;
    }
  }
  return profile;
}

AssetId AssetService::make_asset_id(const FileFingerprint& fingerprint,
                                    const std::filesystem::path& uri) {
  const std::string material = fingerprint.quick_sha256 + "\n" + uri.filename().string();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned digest_length = 0;
  EVP_Digest(material.data(), material.size(), digest.data(), &digest_length, EVP_sha256(), nullptr);
  std::ostringstream value;
  value << std::hex << std::setfill('0');
  for (unsigned index = 0; index < 16U && index < digest_length; ++index) {
    value << std::setw(2) << static_cast<unsigned>(digest[index]);
  }
  return value.str();
}

} // namespace video_editor::assets

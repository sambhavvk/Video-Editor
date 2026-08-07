// SPDX-License-Identifier: MPL-2.0
#include "video_editor/asset_service/asset_service.h"

#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>

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

} // namespace

media::Result<AssetRecord> AssetService::import(const std::filesystem::path& uri,
                                                 const ImportOptions& options) {
  auto fingerprint = fingerprint_file(uri, options.compute_full_hash);
  if (!fingerprint) {
    return media::Result<AssetRecord>::failure(fingerprint.error());
  }
  auto descriptor = media::probe(uri, options.probe);
  if (!descriptor) {
    return media::Result<AssetRecord>::failure(descriptor.error());
  }

  AssetRecord asset{
      .id = make_asset_id(fingerprint.value(), uri),
      .uri = std::filesystem::absolute(uri),
      .fingerprint = fingerprint.value(),
      .descriptor = std::move(descriptor).value(),
      .availability = AssetAvailability::Online,
      .proxy = std::nullopt,
  };
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
    const bool is_4k = stream.video->width >= 3'840 || stream.video->height >= 2'160;
    if (is_4k && is_long_gop_creator_codec(stream.codec_name)) {
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

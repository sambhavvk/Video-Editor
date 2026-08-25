// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/fingerprint.h"
#include "video_editor/media_codec/probe.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::assets {

using AssetId = std::string;

enum class AssetAvailability : std::uint8_t { Online, Missing, Changed };

enum class ProxyCodec : std::uint8_t { ProResProxy, Ffv1 };

struct ProxyProfile {
  ProxyCodec codec{ProxyCodec::ProResProxy};
  int maximum_width{1920};
  int maximum_height{1080};
  bool include_pcm_audio{true};
};

struct ProxyManifest {
  std::filesystem::path proxy_uri;
  std::filesystem::path pts_map_path;
  ProxyProfile profile;
  FileFingerprint source_fingerprint;
  std::string engine_version;
  bool complete{false};
};

struct AssetRecord {
  AssetId id;
  std::filesystem::path uri;
  FileFingerprint fingerprint;
  media::AssetDescriptor descriptor;
  AssetAvailability availability{AssetAvailability::Online};
  std::optional<ProxyManifest> proxy;
};

struct ImportOptions {
  bool compute_full_hash{false};
  media::ProbeOptions probe;
};

class AssetService {
public:
  [[nodiscard]] media::Result<AssetRecord> import(const std::filesystem::path& uri,
                                                   const ImportOptions& options = {});
  [[nodiscard]] media::Result<AssetRecord>
  relink(const AssetRecord& existing, const std::filesystem::path& replacement,
         bool permit_changed_content = false);

  [[nodiscard]] static bool should_recommend_proxy(const AssetRecord& asset) noexcept;
  [[nodiscard]] static ProxyProfile default_proxy_profile(const AssetRecord& asset) noexcept;

private:
  [[nodiscard]] static AssetId make_asset_id(const FileFingerprint& fingerprint,
                                             const std::filesystem::path& uri);
};

} // namespace video_editor::assets


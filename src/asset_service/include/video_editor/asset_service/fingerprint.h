// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace video_editor::assets {

struct FileFingerprint {
  std::uintmax_t size{0};
  std::int64_t modified_nanoseconds{0};
  std::string quick_sha256;
  std::optional<std::string> full_sha256;

  [[nodiscard]] bool content_matches(const FileFingerprint& other) const noexcept {
    return size == other.size && quick_sha256 == other.quick_sha256;
  }
};

[[nodiscard]] media::Result<FileFingerprint>
fingerprint_file(const std::filesystem::path& path, bool include_full_hash = false);

} // namespace video_editor::assets


// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/types.h"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace video_editor::media {

struct ProbeOptions {
  std::int64_t probe_size_bytes{8 * 1024 * 1024};
  std::int64_t analyze_duration_microseconds{10 * 1000 * 1000};
  const std::atomic_bool* cancel{nullptr};
};

[[nodiscard]] Result<AssetDescriptor> probe(const std::filesystem::path& uri,
                                            const ProbeOptions& options = {});

} // namespace video_editor::media


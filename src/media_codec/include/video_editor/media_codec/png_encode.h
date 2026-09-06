// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace video_editor::media {

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_png_rgba8(std::span<const std::uint8_t> rgba,
                                                                   int width, int height);

} // namespace video_editor::media

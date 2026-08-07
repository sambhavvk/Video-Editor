// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "job_service.pb.h"

#include <cstdint>

namespace video_editor::jobs {

inline constexpr std::uint32_t kProtocolMajor = 1;
inline constexpr std::uint32_t kProtocolMinor = 0;
inline constexpr std::uint32_t kMaximumFrameBytes = 64U * 1024U * 1024U;

[[nodiscard]] inline bool compatible(const v1::WorkerRequest& request) noexcept {
  return request.protocol_major() == kProtocolMajor && request.protocol_minor() <= kProtocolMinor;
}

} // namespace video_editor::jobs


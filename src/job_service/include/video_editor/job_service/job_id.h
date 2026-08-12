// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace video_editor::jobs {

[[nodiscard]] std::string make_job_id();
[[nodiscard]] bool valid_job_id(std::string_view value) noexcept;

} // namespace video_editor::jobs


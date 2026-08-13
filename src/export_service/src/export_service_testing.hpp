// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

namespace video_editor::render {
struct VideoFrame;
}

struct AVFrame;

namespace video_editor::export_service::testing {

// Private deterministic seams used only by export safety tests. This header
// is deliberately kept outside the installed export_service include tree.
enum class HardwareFailureInjection : std::uint8_t {
  None,
  HardwareEncode,
  HardwareThenSoftwareEncode,
  HardwareRender,
  SoftwareEncode,
};

void set_hardware_failure_injection(HardwareFailureInjection injection);

[[nodiscard]] bool convert_frame_for_testing(const render::VideoFrame& source, AVFrame& destination,
                                             std::string& message);

} // namespace video_editor::export_service::testing

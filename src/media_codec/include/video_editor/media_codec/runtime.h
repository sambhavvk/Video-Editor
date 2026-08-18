// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace video_editor::media {

struct LibraryVersion {
  unsigned major{0};
  unsigned minor{0};
  unsigned patch{0};
};

struct RuntimeInfo {
  LibraryVersion avformat;
  LibraryVersion avcodec;
  LibraryVersion avutil;
  LibraryVersion swresample;
  LibraryVersion swscale;
  std::string configuration;
  std::string license;
  bool expected_abi{false};
  bool lgpl_compatible_configuration{false};
};

[[nodiscard]] RuntimeInfo runtime_info();

// Process-wide av_log filter for known-harmless decode noise.
void install_quiet_ffmpeg_log_filter();

} // namespace video_editor::media


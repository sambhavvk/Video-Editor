// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/probe.h"

#include <string_view>

extern "C" {
#include <libavformat/avformat.h>
}

namespace video_editor::media {

void apply_input_probe_options(AVFormatContext& context, const ProbeOptions& options = {});
void discard_undecodable_input_streams(AVFormatContext& context);
[[nodiscard]] int inspect_input_streams(AVFormatContext& context);

// Installs a process-wide av_log filter that drops known-harmless decode noise.
void install_quiet_ffmpeg_log_filter();
[[nodiscard]] bool should_suppress_ffmpeg_log(int level, std::string_view message) noexcept;

} // namespace video_editor::media

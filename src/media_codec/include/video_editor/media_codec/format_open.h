// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/probe.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace video_editor::media {

void apply_input_probe_options(AVFormatContext& context, const ProbeOptions& options = {});
void discard_undecodable_input_streams(AVFormatContext& context);
[[nodiscard]] int inspect_input_streams(AVFormatContext& context);

} // namespace video_editor::media

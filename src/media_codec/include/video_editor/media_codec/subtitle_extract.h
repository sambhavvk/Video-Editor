// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_codec/probe.h"
#include "video_editor/media_codec/types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::media {

struct ExtractedSubtitleCue final {
  std::int64_t start_microseconds{0};
  std::int64_t end_microseconds{0};
  std::string text;
};

struct SubtitleStreamInfo final {
  int index{-1};
  std::string codec_name;
  std::string language;
  bool supported_text_codec{false};
};

struct SubtitleExtraction final {
  int stream_index{-1};
  std::string codec_name;
  std::string language;
  std::vector<ExtractedSubtitleCue> cues;
};

struct SubtitleExtractOptions final {
  ProbeOptions probe{};
};

[[nodiscard]] bool is_text_subtitle_codec(std::string_view codec_name) noexcept;
[[nodiscard]] bool is_bitmap_subtitle_codec(std::string_view codec_name) noexcept;
[[nodiscard]] std::vector<SubtitleStreamInfo>
list_subtitle_streams(const AssetDescriptor& asset) noexcept;
[[nodiscard]] Result<SubtitleExtraction>
extract_text_subtitles(const std::filesystem::path& uri, int stream_index,
                       const SubtitleExtractOptions& options = {});

} // namespace video_editor::media

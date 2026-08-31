// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/caption_service/caption_service.h"
#include "video_editor/media_codec/subtitle_extract.h"

#include <filesystem>
#include <string>
#include <vector>

namespace video_editor::caption_service {

struct EmbeddedSubtitleImportOptions final {
  std::string asset_id;
  int stream_index{-1};
};

struct EmbeddedSubtitleError final {
  media::MediaErrorCode code{media::MediaErrorCode::Internal};
  std::string message;

  friend bool operator==(const EmbeddedSubtitleError&, const EmbeddedSubtitleError&) = default;
};

using EmbeddedSubtitleResult = edit::Result<CaptionDocument, EmbeddedSubtitleError>;

[[nodiscard]] std::string embeddedCaptionProvenance(const std::string& asset_id,
                                                    int stream_index);
[[nodiscard]] SubtitleFormat subtitleFormatForCodec(std::string_view codec_name) noexcept;
[[nodiscard]] EmbeddedSubtitleResult
importEmbeddedSubtitles(const std::filesystem::path& uri,
                        const media::AssetDescriptor& asset,
                        const EmbeddedSubtitleImportOptions& options);
[[nodiscard]] std::vector<edit::Caption>
toEditCaptionsFromEmbedded(const CaptionDocument& document);

} // namespace video_editor::caption_service

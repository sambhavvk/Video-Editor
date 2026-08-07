// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include <algorithm>
#include <utility>

namespace video_editor::caption_service {

edit::Caption toEditCaption(const CaptionCue& cue, std::string language,
                            const edit::CaptionStyle& style) {
  edit::Caption caption;
  if (cue.identifier) {
    if (const auto identifier = edit::EntityId::parse(*cue.identifier)) {
      caption.id = *identifier;
    }
  }
  caption.range = cue.range;
  caption.text = cue.text;
  caption.language = std::move(language);
  caption.style = style;
  return caption;
}

CaptionCue fromEditCaption(const edit::Caption& caption) {
  CaptionCue cue;
  cue.identifier = caption.id.toString();
  cue.range = caption.range;
  cue.text = caption.text;
  return cue;
}

std::vector<edit::Caption> toEditCaptions(const CaptionDocument& document,
                                          const edit::CaptionStyle& style) {
  std::vector<edit::Caption> captions;
  captions.reserve(document.cues.size());
  for (const auto& cue : document.cues) {
    captions.push_back(toEditCaption(cue, document.language, style));
  }
  return captions;
}

CaptionDocument fromEditCaptions(std::span<const edit::Caption> captions, SubtitleFormat format,
                                 std::string language) {
  CaptionDocument document;
  document.format = format;
  if (language.empty() && !captions.empty()) {
    const auto& candidate = captions.front().language;
    const auto same_language = std::ranges::all_of(
        captions, [&](const edit::Caption& caption) { return caption.language == candidate; });
    if (same_language) {
      language = candidate;
    }
  }
  document.language = std::move(language);
  document.cues.reserve(captions.size());
  for (const auto& caption : captions) {
    document.cues.push_back(fromEditCaption(caption));
  }
  return document;
}

} // namespace video_editor::caption_service

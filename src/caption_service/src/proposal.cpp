// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] edit::EntityId fragment_id(const edit::EntityId source,
                                         const std::size_t fragment) noexcept {
  auto bytes = source.bytes();
  std::uint64_t value = static_cast<std::uint64_t>(fragment) + 1U;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[8U + index] ^= static_cast<std::uint8_t>(value >> (index * 8U));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x70U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  return edit::EntityId(bytes);
}

[[nodiscard]] edit::Time source_delta(const edit::Clip& clip, const edit::Time timeline_delta) {
  return timeline_delta
      .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
              edit::RoundingMode::NearestTiesEven)
      .rescaledTo(clip.source_range.duration.timescale(), edit::RoundingMode::NearestTiesEven);
}

[[nodiscard]] edit::Time removed_before(const edit::Time time,
                                        std::span<const edit::TimeRange> ranges) {
  auto total = edit::Time{};
  for (const auto& range : ranges) {
    if (range.end() <= time) {
      total = total + range.duration;
    }
  }
  return total;
}

[[nodiscard]] std::optional<edit::TimeRange> map_range(const edit::TimeRange range,
                                                       std::span<const edit::TimeRange> cuts) {
  std::vector<edit::TimeRange> survivors;
  auto cursor = range.start;
  for (const auto& cut : cuts) {
    if (cut.end() <= cursor || cut.start >= range.end())
      continue;
    if (cursor < cut.start)
      survivors.emplace_back(cursor, std::min(cut.start, range.end()) - cursor);
    cursor = std::max(cursor, cut.end());
    if (cursor >= range.end())
      break;
  }
  if (cursor < range.end())
    survivors.emplace_back(cursor, range.end() - cursor);
  if (survivors.empty())
    return std::nullopt;
  const auto start = survivors.front().start - removed_before(survivors.front().start, cuts);
  const auto end = survivors.back().end() - removed_before(survivors.back().end(), cuts);
  return edit::TimeRange(start, end - start);
}

[[nodiscard]] edit::Clip make_fragment(const edit::Clip& source, const edit::Time start,
                                       const edit::Time end, const edit::Time shift,
                                       const std::size_t index, const bool preserve_id,
                                       const std::optional<edit::EntityId> linked_group) {
  edit::Clip result = source;
  if (!preserve_id) {
    result.id = fragment_id(source.id, index);
  }
  result.timeline_range = edit::TimeRange(start - shift, end - start);
  const auto head = start - source.timeline_range.start;
  const auto duration = end - start;
  const auto source_head = source_delta(source, head);
  const auto source_duration = source_delta(source, duration);
  if (source.reversed) {
    result.source_range =
        edit::TimeRange(source.source_range.end() - source_head - source_duration, source_duration);
  } else {
    result.source_range = edit::TimeRange(source.source_range.start + source_head, source_duration);
  }
  result.linked_group = linked_group;
  return result;
}

} // namespace

std::optional<edit::Caption>
mapCaptionThroughCuts(const edit::Caption& caption,
                      const std::span<const edit::TimeRange> selected_ranges) {
  const auto mapped_range = map_range(caption.range, selected_ranges);
  if (!mapped_range)
    return std::nullopt;
  edit::Caption result = caption;
  result.range = *mapped_range;
  result.words.clear();
  result.words.reserve(caption.words.size());
  for (const auto& word : caption.words) {
    const auto mapped = map_range(word.range, selected_ranges);
    if (!mapped)
      continue;
    auto mapped_word = word;
    mapped_word.range = *mapped;
    result.words.push_back(std::move(mapped_word));
  }
  if (!caption.words.empty()) {
    if (result.words.empty())
      return std::nullopt;
    result.text.clear();
    for (std::size_t index = 0; index < result.words.size(); ++index) {
      if (index != 0)
        result.text.push_back(' ');
      result.text += result.words[index].text;
    }
    result.range =
        edit::TimeRange(result.words.front().range.start,
                        result.words.back().range.end() - result.words.front().range.start);
  }
  return result;
}

ProposalResult buildTimelineCutProposal(const edit::TimelineSnapshot& snapshot,
                                        std::span<const edit::TimeRange> selected_ranges) {
  if (selected_ranges.empty()) {
    return ProposalResult::failure(
        {ProposalErrorCode::InvalidRange, "at least one selected range is required"});
  }
  for (std::size_t index = 0; index < selected_ranges.size(); ++index) {
    const auto& range = selected_ranges[index];
    if (range.start.isNegative() || range.duration <= edit::Time{}) {
      return ProposalResult::failure(
          {ProposalErrorCode::InvalidRange, "selected ranges must be positive and non-negative"});
    }
    if (index != 0 && selected_ranges[index - 1].end() > range.start) {
      return ProposalResult::failure(
          {ProposalErrorCode::Overlap, "selected ranges must be sorted and non-overlapping"});
    }
  }

  CaptionProposal proposal;
  proposal.base_revision = snapshot.revision();
  proposal.caption_changes.sequence_id = snapshot.sequence().id;
  proposal.timeline_cuts = edit::ApplyTimelineCutChangeSetCommand{snapshot.sequence().id, {}};
  for (const auto& transition : snapshot.sequence().transitions) {
    const auto* outgoing = snapshot.findClip(transition.outgoing_clip_id);
    const auto* incoming = snapshot.findClip(transition.incoming_clip_id);
    if (std::any_of(
            selected_ranges.begin(), selected_ranges.end(), [&](const edit::TimeRange& range) {
              return range.end() <= transition.range.start || transition.range.overlaps(range) ||
                     (outgoing != nullptr && outgoing->timeline_range.overlaps(range)) ||
                     (incoming != nullptr && incoming->timeline_range.overlaps(range));
            })) {
      return ProposalResult::failure(
          {ProposalErrorCode::UnsupportedTransition,
           "timeline cut proposals do not yet carry transition replacements"});
    }
  }
  for (const auto& caption : snapshot.sequence().captions) {
    const auto mapped = mapCaptionThroughCuts(caption, selected_ranges);
    if (mapped.has_value()) {
      if (*mapped != caption)
        proposal.caption_changes.updated.push_back(*mapped);
    } else {
      proposal.caption_changes.removed.push_back(caption.id);
    }
  }
  for (const auto& range : selected_ranges) {
    proposal.review_items.push_back({range, "Remove selected timeline range"});
  }

  for (const auto& track : snapshot.sequence().tracks) {
    if (track.kind == edit::TrackKind::Caption) {
      continue;
    }
    edit::TrackClipReplacement replacement;
    replacement.track_id = track.id;
    replacement.kind = track.kind;
    std::size_t fragment_index = 0;
    bool affected = false;
    for (const auto& cut : selected_ranges) {
      if (std::any_of(track.clips.begin(), track.clips.end(), [&](const edit::Clip& clip) {
            return clip.timeline_range.overlaps(cut) || clip.timeline_range.start >= cut.end();
          })) {
        affected = true;
        break;
      }
    }
    if (!affected) {
      continue;
    }
    if (track.locked) {
      return ProposalResult::failure(
          {ProposalErrorCode::InvalidSnapshot, "selected cut would modify a locked track"});
    }
    for (const auto& clip : track.clips) {
      std::vector<edit::TimeRange> retained;
      auto cursor = clip.timeline_range.start;
      bool clip_cut = false;
      for (const auto& cut : selected_ranges) {
        if (!clip.timeline_range.overlaps(cut)) {
          continue;
        }
        clip_cut = true;
        if (cursor < cut.start) {
          retained.emplace_back(cursor, cut.start - cursor);
        }
        cursor = std::max(cursor, cut.end());
      }
      if (cursor < clip.timeline_range.end()) {
        retained.emplace_back(cursor, clip.timeline_range.end() - cursor);
      }
      if (retained.empty() && cursor < clip.timeline_range.end()) {
        retained.emplace_back(clip.timeline_range.start, clip.timeline_range.duration);
      }
      for (std::size_t segment_index = 0; segment_index < retained.size(); ++segment_index) {
        const auto& segment = retained[segment_index];
        const auto shift = removed_before(segment.start, selected_ranges);
        const bool preserve_id = !clip_cut;
        std::optional<edit::EntityId> group = clip.linked_group;
        if (group && !preserve_id) {
          group = fragment_id(*group, segment_index);
        }
        replacement.clips.push_back(make_fragment(clip, segment.start, segment.end(), shift,
                                                  fragment_index++, preserve_id, group));
      }
    }
    if (affected) {
      proposal.timeline_cuts->tracks.push_back(std::move(replacement));
    }
  }
  if (proposal.timeline_cuts->tracks.empty()) {
    proposal.timeline_cuts.reset();
  }
  return ProposalResult::success(std::move(proposal));
}

} // namespace video_editor::caption_service

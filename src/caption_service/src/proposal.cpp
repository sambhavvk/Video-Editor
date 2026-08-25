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

struct MappedFragment final {
  edit::EntityId id;
  edit::EntityId track_id;
  edit::TimeRange timeline_range;
  edit::Clip clip;
};

[[nodiscard]] bool has_source_handle_before(const edit::Project& project, const edit::Clip& clip,
                                            const edit::Time timeline_duration) {
  if (clip.kind == edit::ClipKind::Title) {
    return true;
  }
  const auto* asset = edit::findAsset(project, clip.asset_id);
  if (asset == nullptr) {
    return false;
  }
  const auto source_duration =
      timeline_duration
          .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                  edit::RoundingMode::NearestTiesEven)
          .rescaledTo(clip.source_range.duration.timescale(), edit::RoundingMode::NearestTiesEven);
  return clip.reversed ? asset->duration - clip.source_range.end() >= source_duration
                       : clip.source_range.start >= source_duration;
}

[[nodiscard]] bool has_source_handle_after(const edit::Project& project, const edit::Clip& clip,
                                           const edit::Time timeline_duration) {
  if (clip.kind == edit::ClipKind::Title) {
    return true;
  }
  const auto* asset = edit::findAsset(project, clip.asset_id);
  if (asset == nullptr) {
    return false;
  }
  const auto source_duration =
      timeline_duration
          .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                  edit::RoundingMode::NearestTiesEven)
          .rescaledTo(clip.source_range.duration.timescale(), edit::RoundingMode::NearestTiesEven);
  return clip.reversed ? clip.source_range.start >= source_duration
                       : asset->duration - clip.source_range.end() >= source_duration;
}

[[nodiscard]] bool remapped_transition_valid(
    const edit::Project& project, const edit::Transition& transition,
    const std::unordered_map<edit::EntityId, edit::Clip>& post_cut_clips,
    const std::unordered_map<edit::EntityId, edit::EntityId>& clip_track_ids,
    const std::vector<edit::Transition>& accepted) {
  if (transition.id.isNil() || transition.outgoing_clip_id == transition.incoming_clip_id ||
      transition.range.start.isNegative() || transition.range.duration <= edit::Time{}) {
    return false;
  }
  const auto outgoing_it = post_cut_clips.find(transition.outgoing_clip_id);
  const auto incoming_it = post_cut_clips.find(transition.incoming_clip_id);
  if (outgoing_it == post_cut_clips.end() || incoming_it == post_cut_clips.end()) {
    return false;
  }
  const auto outgoing_track_it = clip_track_ids.find(transition.outgoing_clip_id);
  const auto incoming_track_it = clip_track_ids.find(transition.incoming_clip_id);
  if (outgoing_track_it == clip_track_ids.end() ||
      incoming_track_it == clip_track_ids.end() ||
      outgoing_track_it->second != incoming_track_it->second) {
    return false;
  }
  const auto& outgoing = outgoing_it->second;
  const auto& incoming = incoming_it->second;
  const auto cut = outgoing.timeline_range.end();
  if (incoming.timeline_range.start != cut) {
    return false;
  }
  if (transition.range.start >= cut || transition.range.end() <= cut) {
    return false;
  }
  if (transition.range.start < outgoing.timeline_range.start ||
      transition.range.end() > incoming.timeline_range.end()) {
    return false;
  }
  const auto incoming_pre_cut = cut - transition.range.start;
  const auto outgoing_post_cut = transition.range.end() - cut;
  if (!has_source_handle_before(project, incoming, incoming_pre_cut) ||
      !has_source_handle_after(project, outgoing, outgoing_post_cut)) {
    return false;
  }
  if (!transition.enabled) {
    return true;
  }
  const auto& track_id = outgoing_track_it->second;
  for (const auto& other : accepted) {
    if (!other.enabled || other.id == transition.id) {
      continue;
    }
    const auto other_outgoing_track = clip_track_ids.find(other.outgoing_clip_id);
    if (other_outgoing_track == clip_track_ids.end() ||
        other_outgoing_track->second != track_id) {
      continue;
    }
    if (other.range.overlaps(transition.range)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::vector<edit::Transition>
remap_transitions(const edit::TimelineSnapshot& snapshot,
                  std::span<const edit::TimeRange> selected_ranges,
                  const std::unordered_map<edit::EntityId, std::vector<MappedFragment>>&
                      fragments_by_source) {
  std::unordered_map<edit::EntityId, edit::Clip> post_cut_clips;
  std::unordered_map<edit::EntityId, edit::EntityId> clip_track_ids;
  for (const auto& [source_id, fragments] : fragments_by_source) {
    for (const auto& fragment : fragments) {
      post_cut_clips.emplace(fragment.id, fragment.clip);
      clip_track_ids.emplace(fragment.id, fragment.track_id);
    }
    (void)source_id;
  }
  std::vector<edit::Transition> remapped;
  remapped.reserve(snapshot.sequence().transitions.size());
  for (const auto& transition : snapshot.sequence().transitions) {
    const auto mapped_range = map_range(transition.range, selected_ranges);
    if (!mapped_range) {
      continue;
    }
    const auto outgoing_fragments = fragments_by_source.find(transition.outgoing_clip_id);
    const auto incoming_fragments = fragments_by_source.find(transition.incoming_clip_id);
    if (outgoing_fragments == fragments_by_source.end() ||
        incoming_fragments == fragments_by_source.end()) {
      continue;
    }
    bool found_pair = false;
    edit::EntityId new_outgoing;
    edit::EntityId new_incoming;
    for (const auto& outgoing : outgoing_fragments->second) {
      for (const auto& incoming : incoming_fragments->second) {
        const auto junction = outgoing.timeline_range.end();
        if (incoming.timeline_range.start != junction) {
          continue;
        }
        if (mapped_range->start >= junction || mapped_range->end() <= junction) {
          continue;
        }
        new_outgoing = outgoing.id;
        new_incoming = incoming.id;
        found_pair = true;
        break;
      }
      if (found_pair) {
        break;
      }
    }
    if (!found_pair) {
      continue;
    }
    edit::Transition remapped_transition = transition;
    remapped_transition.range = *mapped_range;
    remapped_transition.outgoing_clip_id = new_outgoing;
    remapped_transition.incoming_clip_id = new_incoming;
    if (!remapped_transition_valid(snapshot.project(), remapped_transition, post_cut_clips,
                                     clip_track_ids, remapped)) {
      continue;
    }
    remapped.push_back(remapped_transition);
  }
  return remapped;
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
  proposal.timeline_cuts =
      edit::ApplyTimelineCutChangeSetCommand{snapshot.sequence().id, {}, std::nullopt};
  std::unordered_map<edit::EntityId, std::vector<MappedFragment>> fragments_by_source;
  std::unordered_set<edit::EntityId> replaced_tracks;
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
    replaced_tracks.insert(track.id);
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
        auto fragment = make_fragment(clip, segment.start, segment.end(), shift, fragment_index++,
                                      preserve_id, group);
        replacement.clips.push_back(fragment);
        fragments_by_source[clip.id].push_back(
            MappedFragment{fragment.id, track.id, fragment.timeline_range, fragment});
      }
    }
    if (affected) {
      proposal.timeline_cuts->tracks.push_back(std::move(replacement));
    }
  }
  for (const auto& track : snapshot.sequence().tracks) {
    if (track.kind == edit::TrackKind::Caption || replaced_tracks.contains(track.id)) {
      continue;
    }
    for (const auto& clip : track.clips) {
      fragments_by_source[clip.id].push_back(
          MappedFragment{clip.id, track.id, clip.timeline_range, clip});
    }
  }
  if (proposal.timeline_cuts->tracks.empty()) {
    proposal.timeline_cuts.reset();
  } else {
    proposal.timeline_cuts->transitions =
        remap_transitions(snapshot, selected_ranges, fragments_by_source);
  }
  return ProposalResult::success(std::move(proposal));
}

} // namespace video_editor::caption_service

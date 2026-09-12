// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/sequence_compare.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace video_editor::edit {
namespace {

struct ClipSnapshot final {
  EntityId id{};
  EntityId asset_id{};
  EntityId track_id{};
  std::string name;
  TimeRange timeline_range{};
  TimeRange source_range{};
  Rate playback_rate{1, 1};
  bool reversed{false};
};

[[nodiscard]] std::string clip_label(const ClipSnapshot& clip) {
  return clip.name.empty() ? clip.id.toString() : clip.name;
}

[[nodiscard]] std::vector<ClipSnapshot> collect_clips(const Sequence& sequence) {
  std::vector<ClipSnapshot> clips;
  for (const Track& track : sequence.tracks) {
    for (const Clip& clip : track.clips) {
      clips.push_back(ClipSnapshot{.id = clip.id,
                                   .asset_id = clip.asset_id,
                                   .track_id = track.id,
                                   .name = clip.name,
                                   .timeline_range = clip.timeline_range,
                                   .source_range = clip.source_range,
                                   .playback_rate = clip.playback_rate,
                                   .reversed = clip.reversed});
    }
  }
  return clips;
}

[[nodiscard]] bool transition_lists_match(const Sequence& before, const Sequence& after) {
  if (before.transitions.size() != after.transitions.size()) {
    return false;
  }
  using Signature = std::tuple<int, bool, Time, Time>;
  std::vector<Signature> left;
  std::vector<Signature> right;
  left.reserve(before.transitions.size());
  right.reserve(after.transitions.size());
  for (const Transition& transition : before.transitions) {
    left.emplace_back(static_cast<int>(transition.kind), transition.enabled,
                      transition.range.start, transition.range.duration);
  }
  for (const Transition& transition : after.transitions) {
    right.emplace_back(static_cast<int>(transition.kind), transition.enabled,
                       transition.range.start, transition.range.duration);
  }
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

} // namespace

std::vector<SequenceChange> compare_sequences(const Sequence& before, const Sequence& after) {
  auto before_clips = collect_clips(before);
  auto after_clips = collect_clips(after);
  std::vector<char> used_before(before_clips.size(), 0);
  std::vector<char> used_after(after_clips.size(), 0);
  std::vector<std::pair<std::size_t, std::size_t>> pairs;

  const auto match_if = [&](auto predicate) {
    for (std::size_t before_index = 0; before_index < before_clips.size(); ++before_index) {
      if (used_before[before_index] != 0) {
        continue;
      }
      for (std::size_t after_index = 0; after_index < after_clips.size(); ++after_index) {
        if (used_after[after_index] != 0) {
          continue;
        }
        if (predicate(before_clips[before_index], after_clips[after_index])) {
          used_before[before_index] = 1;
          used_after[after_index] = 1;
          pairs.emplace_back(before_index, after_index);
          break;
        }
      }
    }
  };

  match_if([](const ClipSnapshot& left, const ClipSnapshot& right) { return left.id == right.id; });
  match_if([](const ClipSnapshot& left, const ClipSnapshot& right) {
    return !left.asset_id.isNil() && left.asset_id == right.asset_id && left.name == right.name &&
           left.source_range == right.source_range &&
           left.timeline_range.start == right.timeline_range.start;
  });
  match_if([](const ClipSnapshot& left, const ClipSnapshot& right) {
    return !left.asset_id.isNil() && left.asset_id == right.asset_id && left.name == right.name &&
           left.source_range == right.source_range;
  });
  match_if([](const ClipSnapshot& left, const ClipSnapshot& right) {
    if (!left.name.empty() && left.name == right.name) {
      return left.asset_id == right.asset_id;
    }
    return false;
  });

  std::vector<SequenceChange> changes;
  for (std::size_t after_index = 0; after_index < after_clips.size(); ++after_index) {
    if (used_after[after_index] == 0) {
      changes.push_back(
          {SequenceChangeKind::ClipAdded, clip_label(after_clips[after_index]), "clip added"});
    }
  }
  for (const auto& [before_index, after_index] : pairs) {
    const ClipSnapshot& left = before_clips[before_index];
    const ClipSnapshot& right = after_clips[after_index];
    if (left.track_id != right.track_id || left.timeline_range.start != right.timeline_range.start) {
      changes.push_back(
          {SequenceChangeKind::ClipMoved, clip_label(right), "timeline position changed"});
    }
    if (left.timeline_range.duration != right.timeline_range.duration) {
      changes.push_back(
          {SequenceChangeKind::ClipMoved, clip_label(right), "timeline range changed"});
    }
    if (left.source_range != right.source_range) {
      changes.push_back({SequenceChangeKind::ClipMoved, clip_label(right), "source range changed"});
    }
    if (left.playback_rate != right.playback_rate || left.reversed != right.reversed) {
      changes.push_back(
          {SequenceChangeKind::Unsupported, clip_label(right), "retime is unsupported"});
    }
  }
  for (std::size_t before_index = 0; before_index < before_clips.size(); ++before_index) {
    if (used_before[before_index] == 0) {
      changes.push_back({SequenceChangeKind::ClipRemoved, clip_label(before_clips[before_index]),
                         "clip removed"});
    }
  }
  if (!transition_lists_match(before, after)) {
    changes.push_back(
        {SequenceChangeKind::Unsupported, {}, "unsupported change: transitions"});
  }
  return changes;
}

} // namespace video_editor::edit

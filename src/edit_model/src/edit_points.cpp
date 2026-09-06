// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_points.h"

#include <algorithm>
#include <set>

namespace video_editor::edit {
namespace {

void appendUniqueTime(std::set<Time>& points, Time time) {
  if (!time.isNegative()) {
    points.insert(time);
  }
}

void appendGapPoints(std::set<Time>& points, const Sequence& sequence, const Track& track) {
  const auto limit = sequenceDuration(sequence);
  auto cursor = Time{};
  for (const auto& clip : track.clips) {
    if (clip.timeline_range.start >= limit) {
      break;
    }
    if (clip.timeline_range.start > cursor) {
      appendUniqueTime(points, cursor);
      appendUniqueTime(points, clip.timeline_range.start);
    }
    cursor = std::max(cursor, clip.timeline_range.end());
    if (cursor >= limit) {
      return;
    }
  }
  if (cursor < limit) {
    appendUniqueTime(points, cursor);
    appendUniqueTime(points, limit);
  }
}

[[nodiscard]] bool trackMatchesQuery(const Track& track, const CoveringClipQuery& query) {
  if (query.unlocked_tracks_only && track.locked) {
    return false;
  }
  if (query.targeted_tracks_only && !track.targeted) {
    return false;
  }
  return true;
}

} // namespace

std::vector<Time> collectEditPoints(const Sequence& sequence) {
  std::set<Time> points;
  appendUniqueTime(points, Time{});

  for (const auto& marker : sequence.markers) {
    appendUniqueTime(points, marker.range.start);
    if (!marker.range.empty()) {
      appendUniqueTime(points, marker.range.end());
    }
  }

  for (const auto& transition : sequence.transitions) {
    if (!transition.enabled) {
      continue;
    }
    appendUniqueTime(points, transition.range.start);
    appendUniqueTime(points, transition.range.end());
  }

  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      appendUniqueTime(points, clip.timeline_range.start);
      appendUniqueTime(points, clip.timeline_range.end());
    }
    appendGapPoints(points, sequence, track);
  }

  return {points.begin(), points.end()};
}

std::optional<Time> previousEditPoint(const Sequence& sequence, Time playhead) {
  const auto points = collectEditPoints(sequence);
  std::optional<Time> result;
  for (const auto& point : points) {
    if (point < playhead) {
      result = point;
    } else {
      break;
    }
  }
  return result;
}

std::optional<Time> nextEditPoint(const Sequence& sequence, Time playhead) {
  const auto points = collectEditPoints(sequence);
  for (const auto& point : points) {
    if (point > playhead) {
      return point;
    }
  }
  return std::nullopt;
}

std::vector<EntityId> clipsCoveringPlayhead(const Sequence& sequence,
                                            const CoveringClipQuery& query) {
  std::vector<EntityId> result;
  for (const auto& track : sequence.tracks) {
    if (!trackMatchesQuery(track, query)) {
      continue;
    }
    for (const auto& clip : track.clips) {
      if (clip.timeline_range.contains(query.playhead)) {
        result.push_back(clip.id);
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

Time sourceTimeAtTimelineTime(const Clip& clip, Time timeline_time) {
  Time offset = timeline_time - clip.timeline_range.start;
  offset = offset.scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                         RoundingMode::NearestTiesEven);
  return clip.reversed ? clip.source_range.end() - offset : clip.source_range.start + offset;
}

} // namespace video_editor::edit

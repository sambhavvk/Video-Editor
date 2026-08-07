// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/snapping.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace video_editor::edit {
namespace {

[[nodiscard]] Time absoluteDistance(Time lhs, Time rhs) {
  return lhs < rhs ? rhs - lhs : lhs - rhs;
}

[[nodiscard]] int priority(SnapTargetKind kind) noexcept {
  switch (kind) {
  case SnapTargetKind::Playhead:
    return 0;
  case SnapTargetKind::Marker:
    return 1;
  case SnapTargetKind::ClipEdge:
    return 2;
  case SnapTargetKind::FrameGrid:
    return 3;
  }
  return 4;
}

void appendIfNear(std::vector<SnapCandidate>& candidates, const SnapRequest& request,
                  SnapCandidate candidate) {
  candidate.distance = absoluteDistance(candidate.time, request.proposed_time);
  if (candidate.distance <= request.threshold) {
    candidates.push_back(std::move(candidate));
  }
}

[[nodiscard]] EntityId identityOrNil(const std::optional<EntityId>& identity) noexcept {
  return identity.value_or(EntityId{});
}

[[nodiscard]] int edgePriority(SnapEdge edge) noexcept {
  switch (edge) {
  case SnapEdge::None:
    return 0;
  case SnapEdge::Start:
    return 1;
  case SnapEdge::End:
    return 2;
  }
  return 3;
}

} // namespace

std::vector<SnapCandidate> findSnapCandidates(const Sequence& sequence,
                                              const SnapRequest& request) {
  if (request.threshold.isNegative()) {
    throw std::invalid_argument("snap threshold cannot be negative");
  }

  std::vector<SnapCandidate> candidates;
  if (request.playhead) {
    appendIfNear(candidates, request,
                 SnapCandidate{*request.playhead, Time{}, SnapTargetKind::Playhead, SnapEdge::None,
                               std::nullopt, std::nullopt, std::nullopt});
  }

  if (request.include_markers) {
    for (const auto& marker : sequence.markers) {
      appendIfNear(candidates, request,
                   SnapCandidate{marker.range.start, Time{}, SnapTargetKind::Marker,
                                 SnapEdge::Start, marker.id, std::nullopt, std::nullopt});
      if (!marker.range.empty()) {
        appendIfNear(candidates, request,
                     SnapCandidate{marker.range.end(), Time{}, SnapTargetKind::Marker,
                                   SnapEdge::End, marker.id, std::nullopt, std::nullopt});
      }
    }
  }

  if (request.include_clip_edges) {
    for (const auto& track : sequence.tracks) {
      for (const auto& clip : track.clips) {
        appendIfNear(candidates, request,
                     SnapCandidate{clip.timeline_range.start, Time{}, SnapTargetKind::ClipEdge,
                                   SnapEdge::Start, clip.id, track.id, std::nullopt});
        appendIfNear(candidates, request,
                     SnapCandidate{clip.timeline_range.end(), Time{}, SnapTargetKind::ClipEdge,
                                   SnapEdge::End, clip.id, track.id, std::nullopt});
      }
    }
  }

  if (request.include_frame_grid) {
    const auto floor_frame =
        sequence.frame_rate.framesAt(request.proposed_time, RoundingMode::Floor);
    const auto ceil_frame = sequence.frame_rate.framesAt(request.proposed_time, RoundingMode::Ceil);
    const auto append_frame = [&](std::int64_t frame_number) {
      if (frame_number < 0) {
        return;
      }
      appendIfNear(candidates, request,
                   SnapCandidate{sequence.frame_rate.frameTime(frame_number), Time{},
                                 SnapTargetKind::FrameGrid, SnapEdge::None, std::nullopt,
                                 std::nullopt, frame_number});
    };
    append_frame(floor_frame);
    if (ceil_frame != floor_frame) {
      append_frame(ceil_frame);
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const SnapCandidate& lhs, const SnapCandidate& rhs) {
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              if (priority(lhs.kind) != priority(rhs.kind)) {
                return priority(lhs.kind) < priority(rhs.kind);
              }
              if (lhs.time != rhs.time) {
                return lhs.time < rhs.time;
              }
              if (identityOrNil(lhs.entity_id) != identityOrNil(rhs.entity_id)) {
                return identityOrNil(lhs.entity_id) < identityOrNil(rhs.entity_id);
              }
              if (identityOrNil(lhs.track_id) != identityOrNil(rhs.track_id)) {
                return identityOrNil(lhs.track_id) < identityOrNil(rhs.track_id);
              }
              if (edgePriority(lhs.edge) != edgePriority(rhs.edge)) {
                return edgePriority(lhs.edge) < edgePriority(rhs.edge);
              }
              return lhs.frame_number.value_or(0) < rhs.frame_number.value_or(0);
            });
  return candidates;
}

std::optional<SnapCandidate> nearestSnapCandidate(const Sequence& sequence,
                                                  const SnapRequest& request) {
  auto candidates = findSnapCandidates(sequence, request);
  if (candidates.empty()) {
    return std::nullopt;
  }
  return candidates.front();
}

} // namespace video_editor::edit

// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <optional>
#include <unordered_set>
#include <vector>

namespace video_editor::edit {

// Equal-distance candidates use this stable priority order: playhead, marker,
// clip edge, then frame grid. Candidates of the same kind are ordered by time,
// identity, track, and edge. No conversion to floating point is performed.
enum class SnapTargetKind { Playhead, Marker, ClipEdge, FrameGrid };
enum class SnapEdge { None, Start, End };

struct SnapRequest final {
  Time proposed_time{};
  Time threshold{};
  std::optional<Time> playhead;
  bool include_clip_edges{true};
  bool include_markers{true};
  bool include_frame_grid{true};
  // Targets belonging to the active interaction are ignored so it cannot
  // snap to its own former position. Clip-edge exclusion applies to both
  // edges of each selected clip; marker exclusion applies to both marker
  // range boundaries.
  std::unordered_set<EntityId> excluded_clip_ids;
  std::unordered_set<EntityId> excluded_marker_ids;
};

struct SnapCandidate final {
  Time time{};
  Time distance{};
  SnapTargetKind kind{SnapTargetKind::FrameGrid};
  SnapEdge edge{SnapEdge::None};
  std::optional<EntityId> entity_id;
  std::optional<EntityId> track_id;
  std::optional<std::int64_t> frame_number;

  friend bool operator==(const SnapCandidate&, const SnapCandidate&) = default;
};

// Returns every target inside the inclusive threshold, nearest first.
[[nodiscard]] std::vector<SnapCandidate> findSnapCandidates(const Sequence& sequence,
                                                            const SnapRequest& request);

[[nodiscard]] std::optional<SnapCandidate> nearestSnapCandidate(const Sequence& sequence,
                                                                const SnapRequest& request);

} // namespace video_editor::edit

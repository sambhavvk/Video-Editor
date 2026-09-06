// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <optional>
#include <vector>

namespace video_editor::edit {

struct CoveringClipQuery final {
  Time playhead{};
  bool targeted_tracks_only{true};
  bool unlocked_tracks_only{true};
};

// Collects every editorial boundary: clip edges, transition edges, marker
// boundaries, and gap edges. Times are deduplicated and sorted ascending.
[[nodiscard]] std::vector<Time> collectEditPoints(const Sequence& sequence);

[[nodiscard]] std::optional<Time> previousEditPoint(const Sequence& sequence, Time playhead);
[[nodiscard]] std::optional<Time> nextEditPoint(const Sequence& sequence, Time playhead);

// Clips whose timeline range contains playhead on tracks matching the query.
[[nodiscard]] std::vector<EntityId> clipsCoveringPlayhead(const Sequence& sequence,
                                                          const CoveringClipQuery& query);

// Maps a timeline time inside clip to the corresponding source time.
[[nodiscard]] Time sourceTimeAtTimelineTime(const Clip& clip, Time timeline_time);

} // namespace video_editor::edit

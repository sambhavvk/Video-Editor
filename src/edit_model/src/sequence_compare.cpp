// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/sequence_compare.h"

#include <unordered_map>

namespace video_editor::edit {

std::vector<SequenceChange> compare_sequences(const Sequence& before, const Sequence& after) {
  std::unordered_map<std::string, Time> before_positions;
  for (const Track& track : before.tracks) {
    for (const Clip& clip : track.clips) {
      before_positions.emplace(clip.id.toString(), clip.timeline_range.start);
    }
  }

  std::vector<SequenceChange> changes;
  for (const Track& track : after.tracks) {
    for (const Clip& clip : track.clips) {
      const auto found = before_positions.find(clip.id.toString());
      if (found == before_positions.end()) {
        changes.push_back({SequenceChangeKind::ClipAdded, clip.name, "clip added"});
        continue;
      }
      if (found->second != clip.timeline_range.start) {
        changes.push_back({SequenceChangeKind::ClipMoved, clip.name, "timeline position changed"});
      }
      before_positions.erase(found);
    }
  }
  for (const auto& [clip_id, start] : before_positions) {
    static_cast<void>(start);
    changes.push_back({SequenceChangeKind::ClipRemoved, clip_id, "clip removed"});
  }
  return changes;
}

} // namespace video_editor::edit

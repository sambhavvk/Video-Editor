// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/model.h"

#include <algorithm>

namespace video_editor::edit {

const Asset* findAsset(const Project& project, EntityId id) noexcept {
  const auto found = std::find_if(project.assets.begin(), project.assets.end(),
                                  [id](const Asset& asset) { return asset.id == id; });
  return found == project.assets.end() ? nullptr : &*found;
}

const MediaBin* findBin(const Project& project, EntityId id) noexcept {
  const auto found = std::find_if(project.bins.begin(), project.bins.end(),
                                  [id](const MediaBin& bin) { return bin.id == id; });
  return found == project.bins.end() ? nullptr : &*found;
}

const MulticamGroup* findMulticamGroup(const Project& project, EntityId id) noexcept {
  const auto found =
      std::find_if(project.multicam_groups.begin(), project.multicam_groups.end(),
                   [id](const MulticamGroup& group) { return group.id == id; });
  return found == project.multicam_groups.end() ? nullptr : &*found;
}

const MulticamGroup* findMulticamGroupForClip(const Project& project,
                                              EntityId clip_id) noexcept {
  for (const auto& group : project.multicam_groups) {
    for (const auto& angle : group.angles) {
      if (angle.clip_id == clip_id) {
        return &group;
      }
    }
  }
  return nullptr;
}

const MulticamAngle* findMulticamAngle(const MulticamGroup& group, EntityId angle_id) noexcept {
  const auto found =
      std::find_if(group.angles.begin(), group.angles.end(),
                   [angle_id](const MulticamAngle& angle) { return angle.id == angle_id; });
  return found == group.angles.end() ? nullptr : &*found;
}

EntityId activeMulticamAngleId(const MulticamGroup& group, Time time) noexcept {
  const MulticamSwitch* selected = nullptr;
  for (const MulticamSwitch& entry : group.switches) {
    if (entry.time > time) {
      continue;
    }
    if (selected == nullptr || entry.time >= selected->time) {
      selected = &entry;
    }
  }
  if (selected != nullptr) {
    return selected->angle_id;
  }
  return group.active_angle_id;
}

const MulticamGroup* findMulticamGroupForSequenceClip(const Project& project,
                                                      EntityId sequence_id,
                                                      EntityId clip_id) noexcept {
  const MulticamGroup* group = findMulticamGroupForClip(project, clip_id);
  if (group == nullptr || group->sequence_id != sequence_id) {
    return nullptr;
  }
  return group;
}

bool multicamVideoClipVisible(const Project& project, EntityId sequence_id, const Clip& clip,
                              Time time) noexcept {
  const MulticamGroup* group = findMulticamGroupForSequenceClip(project, sequence_id, clip.id);
  if (group == nullptr) {
    return true;
  }
  const MulticamAngle* active =
      findMulticamAngle(*group, activeMulticamAngleId(*group, time));
  return active != nullptr && active->clip_id == clip.id;
}

bool multicamAudioClipAudible(const Project& project, const Sequence& sequence,
                              const Clip& clip) noexcept {
  for (const MulticamGroup& group : project.multicam_groups) {
    if (group.sequence_id != sequence.id) {
      continue;
    }
    const MulticamAngle* master = findMulticamAngle(group, group.audio_master_angle_id);
    if (master == nullptr) {
      continue;
    }
    const auto clip_in_angle = [&](const MulticamAngle& angle) -> bool {
      const Clip* angle_clip = findClip(sequence, angle.clip_id);
      if (angle_clip == nullptr) {
        return false;
      }
      if (clip.id == angle_clip->id) {
        return true;
      }
      return angle_clip->linked_group.has_value() &&
             clip.linked_group == angle_clip->linked_group;
    };
    if (clip_in_angle(*master)) {
      return true;
    }
    for (const MulticamAngle& angle : group.angles) {
      if (angle.id == group.audio_master_angle_id) {
        continue;
      }
      if (clip_in_angle(angle)) {
        return false;
      }
    }
  }
  return true;
}

const Sequence* findSequence(const Project& project, EntityId id) noexcept {
  const auto found = std::find_if(project.sequences.begin(), project.sequences.end(),
                                  [id](const Sequence& sequence) { return sequence.id == id; });
  return found == project.sequences.end() ? nullptr : &*found;
}

const Track* findTrack(const Sequence& sequence, EntityId id) noexcept {
  const auto found = std::find_if(sequence.tracks.begin(), sequence.tracks.end(),
                                  [id](const Track& track) { return track.id == id; });
  return found == sequence.tracks.end() ? nullptr : &*found;
}

const Clip* findClip(const Sequence& sequence, EntityId id) noexcept {
  for (const auto& track : sequence.tracks) {
    const auto found = std::find_if(track.clips.begin(), track.clips.end(),
                                    [id](const Clip& clip) { return clip.id == id; });
    if (found != track.clips.end()) {
      return &*found;
    }
  }
  return nullptr;
}

const Transition* findTransition(const Sequence& sequence, EntityId id) noexcept {
  const auto found =
      std::find_if(sequence.transitions.begin(), sequence.transitions.end(),
                   [id](const Transition& transition) { return transition.id == id; });
  return found == sequence.transitions.end() ? nullptr : &*found;
}

Time sequenceDuration(const Sequence& sequence) {
  auto duration = Time{};
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      duration = std::max(duration, clip.timeline_range.end());
    }
  }
  for (const auto& marker : sequence.markers) {
    duration = std::max(duration, marker.range.end());
  }
  for (const auto& caption : sequence.captions) {
    duration = std::max(duration, caption.range.end());
  }
  for (const auto& transition : sequence.transitions) {
    duration = std::max(duration, transition.range.end());
  }
  return duration;
}

} // namespace video_editor::edit

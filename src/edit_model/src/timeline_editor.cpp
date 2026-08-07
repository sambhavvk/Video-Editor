// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/timeline_editor.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace video_editor::edit {
namespace {

template <class... Callables> struct Overloaded : Callables... {
  using Callables::operator()...;
};
template <class... Callables> Overloaded(Callables...) -> Overloaded<Callables...>;

[[nodiscard]] EditError error(EditErrorCode code, std::string message) {
  return EditError{code, std::move(message), std::nullopt, std::nullopt};
}

[[nodiscard]] Sequence* mutableSequence(Project& project, EntityId id) noexcept {
  const auto found = std::find_if(project.sequences.begin(), project.sequences.end(),
                                  [id](const Sequence& sequence) { return sequence.id == id; });
  return found == project.sequences.end() ? nullptr : &*found;
}

[[nodiscard]] Track* mutableTrack(Sequence& sequence, EntityId id) noexcept {
  const auto found = std::find_if(sequence.tracks.begin(), sequence.tracks.end(),
                                  [id](const Track& track) { return track.id == id; });
  return found == sequence.tracks.end() ? nullptr : &*found;
}

struct ClipLocation final {
  Track* track{nullptr};
  std::vector<Clip>::iterator clip;
};

[[nodiscard]] std::optional<ClipLocation> mutableClip(Sequence& sequence, EntityId id) noexcept {
  for (auto& track : sequence.tracks) {
    const auto found = std::find_if(track.clips.begin(), track.clips.end(),
                                    [id](const Clip& clip) { return clip.id == id; });
    if (found != track.clips.end()) {
      return ClipLocation{&track, found};
    }
  }
  return std::nullopt;
}

void sortClips(Track& track) {
  std::stable_sort(track.clips.begin(), track.clips.end(), [](const Clip& lhs, const Clip& rhs) {
    if (lhs.timeline_range.start == rhs.timeline_range.start) {
      return lhs.id < rhs.id;
    }
    return lhs.timeline_range.start < rhs.timeline_range.start;
  });
}

[[nodiscard]] bool trackAccepts(const Track& track, const Clip& clip) noexcept {
  switch (track.kind) {
  case TrackKind::Video:
    return clip.kind == ClipKind::Video || clip.kind == ClipKind::Title;
  case TrackKind::Audio:
    return clip.kind == ClipKind::Audio;
  case TrackKind::Caption:
    return false;
  }
  return false;
}

[[nodiscard]] bool hasOverlap(const Track& track, const TimeRange& range,
                              std::optional<EntityId> ignored = std::nullopt) {
  return std::any_of(track.clips.begin(), track.clips.end(), [&](const Clip& candidate) {
    return (!ignored || candidate.id != *ignored) && candidate.timeline_range.overlaps(range);
  });
}

[[nodiscard]] bool hasOverlapExcluding(const Track& track, const TimeRange& range,
                                       const std::unordered_set<EntityId>& ignored) {
  return std::any_of(track.clips.begin(), track.clips.end(), [&](const Clip& candidate) {
    return !ignored.contains(candidate.id) && candidate.timeline_range.overlaps(range);
  });
}

[[nodiscard]] std::optional<EditError> validateClip(const Project& project, const Track& track,
                                                    const Clip& clip) {
  if (clip.id.isNil()) {
    return error(EditErrorCode::InvalidArgument, "clip id cannot be nil");
  }
  if (clip.timeline_range.start.isNegative() || clip.timeline_range.duration <= Time{}) {
    return error(EditErrorCode::InvalidArgument,
                 "clip timeline range must have a non-negative start and positive duration");
  }
  if (clip.source_range.start.isNegative() || clip.source_range.duration <= Time{}) {
    return error(EditErrorCode::InvalidArgument,
                 "clip source range must have a non-negative start and positive duration");
  }
  if (!trackAccepts(track, clip)) {
    return error(EditErrorCode::InvalidTrackKind,
                 "clip kind is incompatible with destination track");
  }
  if (clip.kind != ClipKind::Title) {
    const auto* asset = findAsset(project, clip.asset_id);
    if (asset == nullptr) {
      return error(EditErrorCode::EntityNotFound,
                   "clip references an asset that is not in the project");
    }
    if ((clip.kind == ClipKind::Video && !asset->has_video) ||
        (clip.kind == ClipKind::Audio && !asset->has_audio)) {
      return error(EditErrorCode::InvalidTrackKind,
                   "asset does not contain the media required by the clip");
    }
    if (clip.source_range.end() > asset->duration) {
      return error(EditErrorCode::InvalidArgument,
                   "clip source range extends beyond the asset duration");
    }
  }
  if (clip.transform.opacity < 0.0 || clip.transform.opacity > 1.0 || clip.audio_pan < -1.0 ||
      clip.audio_pan > 1.0) {
    return error(EditErrorCode::InvalidArgument,
                 "clip opacity or audio pan is outside its valid range");
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> validateProject(const Project& project) {
  if (project.id.isNil()) {
    return error(EditErrorCode::InvalidArgument, "project id cannot be nil");
  }

  std::unordered_set<EntityId> ids;
  const auto addId = [&ids](EntityId id) { return !id.isNil() && ids.emplace(id).second; };
  if (!addId(project.id)) {
    return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil id");
  }
  for (const auto& asset : project.assets) {
    if (!addId(asset.id)) {
      return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil asset id");
    }
    if (asset.duration.isNegative()) {
      return error(EditErrorCode::InvalidArgument, "asset duration cannot be negative");
    }
  }
  for (const auto& sequence : project.sequences) {
    if (!addId(sequence.id)) {
      return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil sequence id");
    }
    if (sequence.width == 0 || sequence.height == 0 || sequence.audio_sample_rate == 0) {
      return error(EditErrorCode::InvalidArgument,
                   "sequence dimensions and sample rate must be non-zero");
    }
    for (const auto& track : sequence.tracks) {
      if (!addId(track.id)) {
        return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil track id");
      }
      if (track.kind == TrackKind::Caption && !track.clips.empty()) {
        return error(EditErrorCode::InvalidTrackKind, "caption tracks cannot contain media clips");
      }
      const Clip* previous = nullptr;
      for (const auto& clip : track.clips) {
        if (!addId(clip.id)) {
          return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil clip id");
        }
        if (const auto issue = validateClip(project, track, clip)) {
          return issue;
        }
        if (previous != nullptr && previous->timeline_range.overlaps(clip.timeline_range)) {
          return error(EditErrorCode::Overlap, "clips on a track cannot overlap");
        }
        previous = &clip;
        for (const auto& effect : clip.effects) {
          if (!addId(effect.id)) {
            return error(EditErrorCode::DuplicateId,
                         "project contains a duplicate or nil effect id");
          }
        }
      }
      for (const auto& effect : track.effects) {
        if (!addId(effect.id)) {
          return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil effect id");
        }
      }
    }
    for (const auto& marker : sequence.markers) {
      if (!addId(marker.id)) {
        return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil marker id");
      }
      if (marker.range.start.isNegative()) {
        return error(EditErrorCode::InvalidArgument, "marker time cannot be negative");
      }
    }
    for (const auto& caption : sequence.captions) {
      if (!addId(caption.id)) {
        return error(EditErrorCode::DuplicateId, "project contains a duplicate or nil caption id");
      }
      if (caption.range.start.isNegative() || caption.range.duration <= Time{}) {
        return error(EditErrorCode::InvalidArgument,
                     "caption range must have a non-negative start and positive duration");
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> splitClipObject(const Clip& original, Time split_time,
                                                       EntityId right_id, Clip& left, Clip& right) {
  if (right_id.isNil() || split_time <= original.timeline_range.start ||
      split_time >= original.timeline_range.end()) {
    return error(EditErrorCode::InvalidArgument,
                 "split time must be strictly inside the clip and the new id must be non-nil");
  }

  left = original;
  right = original;
  right.id = right_id;
  const auto left_duration = split_time - original.timeline_range.start;
  const auto right_duration = original.timeline_range.end() - split_time;
  auto source_consumed =
      left_duration
          .scaled(original.playback_rate.numerator(), original.playback_rate.denominator(),
                  RoundingMode::NearestTiesEven)
          .rescaledTo(original.source_range.duration.timescale(), RoundingMode::NearestTiesEven);
  if (source_consumed <= Time{} || source_consumed >= original.source_range.duration) {
    return error(EditErrorCode::InvalidArgument,
                 "split does not map to a valid source-media boundary");
  }
  const auto remaining_source = original.source_range.duration - source_consumed;
  left.timeline_range = TimeRange(original.timeline_range.start, left_duration);
  right.timeline_range = TimeRange(split_time, right_duration);
  if (!original.reversed) {
    left.source_range = TimeRange(original.source_range.start, source_consumed);
    right.source_range = TimeRange(original.source_range.start + source_consumed, remaining_source);
  } else {
    left.source_range = TimeRange(original.source_range.end() - source_consumed, source_consumed);
    right.source_range = TimeRange(original.source_range.start, remaining_source);
  }
  return std::nullopt;
}

[[nodiscard]] Time sourceDeltaForTimelineDelta(const Clip& clip, Time timeline_delta) {
  return timeline_delta
      .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
              RoundingMode::NearestTiesEven)
      .rescaledTo(clip.source_range.duration.timescale(), RoundingMode::NearestTiesEven);
}

// Changes timeline boundaries while retaining the media presented at every
// unchanged timeline point. Source ranges are always stored low-to-high, even
// when playback is reversed.
[[nodiscard]] std::optional<EditError>
trimPreservingMapping(const Clip& original, const TimeRange& timeline_range, Clip& trimmed) {
  if (timeline_range.start.isNegative() || timeline_range.duration <= Time{}) {
    return error(EditErrorCode::InvalidArgument,
                 "trimmed timeline range must have a non-negative start and positive duration");
  }

  const auto head_delta = timeline_range.start - original.timeline_range.start;
  const auto tail_delta = timeline_range.end() - original.timeline_range.end();
  const auto head_source_delta = sourceDeltaForTimelineDelta(original, head_delta);
  const auto tail_source_delta = sourceDeltaForTimelineDelta(original, tail_delta);

  auto source_start = original.source_range.start;
  auto source_end = original.source_range.end();
  if (!original.reversed) {
    source_start = source_start + head_source_delta;
    source_end = source_end + tail_source_delta;
  } else {
    source_start = source_start - tail_source_delta;
    source_end = source_end - head_source_delta;
  }
  if (source_start.isNegative() || source_end <= source_start) {
    return error(EditErrorCode::InvalidArgument,
                 "edit requires source media outside the available handles");
  }

  trimmed = original;
  trimmed.timeline_range = timeline_range;
  trimmed.source_range = TimeRange(source_start, source_end - source_start);
  return std::nullopt;
}

[[nodiscard]] std::vector<EntityId> linkedClipIds(const Sequence& sequence, const Clip& primary,
                                                  bool include_linked) {
  if (!include_linked || !primary.linked_group) {
    return {primary.id};
  }
  std::vector<EntityId> ids;
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.linked_group == primary.linked_group) {
        ids.push_back(clip.id);
      }
    }
  }
  return ids;
}

[[nodiscard]] std::unordered_set<EntityId> idSet(const std::vector<EntityId>& ids) {
  return std::unordered_set<EntityId>(ids.begin(), ids.end());
}

struct PlannedClip final {
  EntityId id;
  EntityId track_id;
  Clip clip;
};

[[nodiscard]] std::optional<EditError> insertClip(Project& project, Sequence& sequence,
                                                  Track& track, Clip clip, InsertMode mode);

[[nodiscard]] std::optional<EditError> applyMoveClip(Project& project, Sequence& sequence,
                                                     const MoveClipCommand& command) {
  auto primary_location = mutableClip(sequence, command.clip_id);
  if (!primary_location) {
    return error(EditErrorCode::EntityNotFound, "clip was not found");
  }
  const auto primary = *primary_location->clip;
  const auto delta = command.new_start - primary.timeline_range.start;
  const auto selected_ids = linkedClipIds(sequence, primary, command.include_linked);

  std::vector<PlannedClip> plans;
  plans.reserve(selected_ids.size());
  std::unordered_map<EntityId, std::size_t> destination_counts;
  for (const auto id : selected_ids) {
    auto location = mutableClip(sequence, id);
    if (!location) {
      return error(EditErrorCode::EntityNotFound, "linked clip was not found");
    }
    if (location->track->locked) {
      return error(EditErrorCode::TrackLocked, "cannot move a clip from a locked track");
    }
    const auto destination_id =
        id == command.clip_id ? command.destination_track_id : location->track->id;
    const auto* destination = findTrack(sequence, destination_id);
    if (destination == nullptr) {
      return error(EditErrorCode::EntityNotFound, "destination track was not found");
    }
    if (destination->locked) {
      return error(EditErrorCode::TrackLocked, "cannot move a clip to a locked track");
    }
    auto moved = *location->clip;
    moved.timeline_range.start = moved.timeline_range.start + delta;
    if (const auto issue = validateClip(project, *destination, moved)) {
      return issue;
    }
    plans.push_back(PlannedClip{id, destination_id, std::move(moved)});
    ++destination_counts[destination_id];
  }
  if (command.mode == InsertMode::Ripple &&
      std::any_of(destination_counts.begin(), destination_counts.end(),
                  [](const auto& item) { return item.second > 1; })) {
    return error(EditErrorCode::InvalidArgument,
                 "linked ripple move requires at most one selected clip per destination track");
  }

  const auto selected = idSet(selected_ids);
  for (auto& track : sequence.tracks) {
    std::erase_if(track.clips, [&](const Clip& clip) { return selected.contains(clip.id); });
  }
  for (auto& plan : plans) {
    auto* destination = mutableTrack(sequence, plan.track_id);
    if (destination == nullptr) {
      return error(EditErrorCode::EntityNotFound, "destination track was not found");
    }
    if (auto issue =
            insertClip(project, sequence, *destination, std::move(plan.clip), command.mode)) {
      return issue;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> applyTrimClip(Project& project, Sequence& sequence,
                                                     const TrimClipCommand& command) {
  auto primary_location = mutableClip(sequence, command.clip_id);
  if (!primary_location) {
    return error(EditErrorCode::EntityNotFound, "clip was not found");
  }
  const auto primary = *primary_location->clip;
  const auto selected_ids = linkedClipIds(sequence, primary, command.include_linked);
  const auto selected = idSet(selected_ids);
  const auto head_delta = command.timeline_range.start - primary.timeline_range.start;
  const auto tail_delta = command.timeline_range.end() - primary.timeline_range.end();

  std::vector<PlannedClip> plans;
  plans.reserve(selected_ids.size());
  for (const auto id : selected_ids) {
    auto location = mutableClip(sequence, id);
    if (!location) {
      return error(EditErrorCode::EntityNotFound, "linked clip was not found");
    }
    if (location->track->locked) {
      return error(EditErrorCode::TrackLocked, "cannot trim a clip on a locked track");
    }

    auto trimmed = *location->clip;
    if (id == command.clip_id) {
      trimmed.timeline_range = command.timeline_range;
      trimmed.source_range = command.source_range;
    } else {
      const auto linked_start = trimmed.timeline_range.start + head_delta;
      const auto linked_end = trimmed.timeline_range.end() + tail_delta;
      if (linked_end <= linked_start) {
        return error(EditErrorCode::InvalidArgument,
                     "linked trim would make a companion clip empty");
      }
      if (auto issue = trimPreservingMapping(
              *location->clip, TimeRange(linked_start, linked_end - linked_start), trimmed)) {
        return issue;
      }
    }
    if (const auto issue = validateClip(project, *location->track, trimmed)) {
      return issue;
    }
    if (hasOverlapExcluding(*location->track, trimmed.timeline_range, selected)) {
      return error(EditErrorCode::Overlap, "trimmed clip overlaps another clip");
    }
    plans.push_back(PlannedClip{id, location->track->id, std::move(trimmed)});
  }

  for (auto& plan : plans) {
    auto location = mutableClip(sequence, plan.id);
    if (!location) {
      return error(EditErrorCode::EntityNotFound, "linked clip was not found");
    }
    *location->clip = std::move(plan.clip);
    sortClips(*location->track);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> prepareTrackForInsert(Track& track, const Clip& clip,
                                                             InsertMode mode) {
  if (!hasOverlap(track, clip.timeline_range)) {
    if (mode == InsertMode::Ripple) {
      for (auto& existing : track.clips) {
        if (existing.timeline_range.start >= clip.timeline_range.start) {
          existing.timeline_range.start =
              existing.timeline_range.start + clip.timeline_range.duration;
        } else if (existing.timeline_range.end() > clip.timeline_range.start) {
          return error(EditErrorCode::Overlap, "cannot ripple-insert into the middle of a clip");
        }
      }
    }
    return std::nullopt;
  }

  if (mode == InsertMode::RejectOverlap) {
    return error(EditErrorCode::Overlap, "clip overlaps another clip on the destination track");
  }
  if (mode == InsertMode::Ripple) {
    return error(EditErrorCode::Overlap, "cannot ripple-insert into the middle of a clip");
  }

  std::vector<Clip> retained;
  retained.reserve(track.clips.size() + 1);
  for (const auto& existing : track.clips) {
    if (!existing.timeline_range.overlaps(clip.timeline_range)) {
      retained.push_back(existing);
      continue;
    }

    const bool keep_left = existing.timeline_range.start < clip.timeline_range.start;
    const bool keep_right = existing.timeline_range.end() > clip.timeline_range.end();
    if (keep_left && keep_right) {
      Clip left;
      Clip after_left;
      if (auto issue = splitClipObject(existing, clip.timeline_range.start, EntityId::generate(),
                                       left, after_left)) {
        return issue;
      }
      Clip covered;
      Clip right;
      if (auto issue = splitClipObject(after_left, clip.timeline_range.end(), EntityId::generate(),
                                       covered, right)) {
        return issue;
      }
      retained.push_back(std::move(left));
      retained.push_back(std::move(right));
    } else if (keep_left) {
      Clip left;
      Clip covered;
      if (auto issue = splitClipObject(existing, clip.timeline_range.start, EntityId::generate(),
                                       left, covered)) {
        return issue;
      }
      retained.push_back(std::move(left));
    } else if (keep_right) {
      Clip covered;
      Clip right;
      if (auto issue = splitClipObject(existing, clip.timeline_range.end(), EntityId::generate(),
                                       covered, right)) {
        return issue;
      }
      right.id = existing.id;
      retained.push_back(std::move(right));
    }
  }
  track.clips = std::move(retained);
  sortClips(track);
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> insertClip(Project& project, Sequence& sequence,
                                                  Track& track, Clip clip, InsertMode mode) {
  if (track.locked) {
    return error(EditErrorCode::TrackLocked, "cannot insert a clip into a locked track");
  }
  if (findClip(sequence, clip.id) != nullptr) {
    return error(EditErrorCode::DuplicateId,
                 "a clip with the same id already exists in the sequence");
  }
  if (const auto issue = validateClip(project, track, clip)) {
    return issue;
  }
  if (auto issue = prepareTrackForInsert(track, clip, mode)) {
    return issue;
  }
  track.clips.push_back(std::move(clip));
  sortClips(track);
  return std::nullopt;
}

[[nodiscard]] std::optional<EditError> applyOperation(Project& project, const EditOperation& op) {
  return std::visit(
      Overloaded{
          [&](const AddAssetCommand& command) -> std::optional<EditError> {
            if (command.asset.id.isNil()) {
              return error(EditErrorCode::InvalidArgument, "asset id cannot be nil");
            }
            if (findAsset(project, command.asset.id) != nullptr) {
              return error(EditErrorCode::DuplicateId, "an asset with the same id already exists");
            }
            project.assets.push_back(command.asset);
            return std::nullopt;
          },
          [&](const RemoveAssetCommand& command) -> std::optional<EditError> {
            const auto found =
                std::find_if(project.assets.begin(), project.assets.end(),
                             [&](const Asset& asset) { return asset.id == command.asset_id; });
            if (found == project.assets.end()) {
              return error(EditErrorCode::EntityNotFound, "asset was not found");
            }
            for (const auto& sequence : project.sequences) {
              for (const auto& track : sequence.tracks) {
                if (std::any_of(track.clips.begin(), track.clips.end(), [&](const Clip& clip) {
                      return clip.asset_id == command.asset_id;
                    })) {
                  return error(EditErrorCode::AssetInUse,
                               "asset cannot be removed while clips reference it");
                }
              }
            }
            project.assets.erase(found);
            return std::nullopt;
          },
          [&](const AddSequenceCommand& command) -> std::optional<EditError> {
            if (command.sequence.id.isNil()) {
              return error(EditErrorCode::InvalidArgument, "sequence id cannot be nil");
            }
            if (findSequence(project, command.sequence.id) != nullptr) {
              return error(EditErrorCode::DuplicateId,
                           "a sequence with the same id already exists");
            }
            project.sequences.push_back(command.sequence);
            return std::nullopt;
          },
          [&](const RemoveSequenceCommand& command) -> std::optional<EditError> {
            const auto found = std::find_if(
                project.sequences.begin(), project.sequences.end(),
                [&](const Sequence& sequence) { return sequence.id == command.sequence_id; });
            if (found == project.sequences.end()) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            project.sequences.erase(found);
            return std::nullopt;
          },
          [&](const SetSequenceFormatCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            if (command.frame_rate.numerator() == 0 || command.frame_rate.denominator() == 0) {
              return error(EditErrorCode::InvalidArgument,
                           "sequence frame rate components must be non-zero");
            }
            if (command.width == 0 || command.height == 0) {
              return error(EditErrorCode::InvalidArgument,
                           "sequence canvas dimensions must be non-zero");
            }
            sequence->frame_rate = command.frame_rate;
            sequence->width = command.width;
            sequence->height = command.height;
            return std::nullopt;
          },
          [&](const AddTrackCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            if (findTrack(*sequence, command.track.id) != nullptr) {
              return error(EditErrorCode::DuplicateId, "a track with the same id already exists");
            }
            if (command.index && *command.index > sequence->tracks.size()) {
              return error(EditErrorCode::InvalidArgument, "track insertion index is out of range");
            }
            const auto position = command.index.value_or(sequence->tracks.size());
            sequence->tracks.insert(
                sequence->tracks.begin() + static_cast<std::ptrdiff_t>(position), command.track);
            return std::nullopt;
          },
          [&](const RemoveTrackCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            const auto found =
                std::find_if(sequence->tracks.begin(), sequence->tracks.end(),
                             [&](const Track& track) { return track.id == command.track_id; });
            if (found == sequence->tracks.end()) {
              return error(EditErrorCode::EntityNotFound, "track was not found");
            }
            if (found->locked) {
              return error(EditErrorCode::TrackLocked, "cannot remove a locked track");
            }
            sequence->tracks.erase(found);
            return std::nullopt;
          },
          [&](const InsertClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto* track = mutableTrack(*sequence, command.track_id);
            if (track == nullptr) {
              return error(EditErrorCode::EntityNotFound, "track was not found");
            }
            return insertClip(project, *sequence, *track, command.clip, command.mode);
          },
          [&](const MoveClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            return applyMoveClip(project, *sequence, command);
          },
          [&](const TrimClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            return applyTrimClip(project, *sequence, command);
          },
          [&](const SplitClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            if (findClip(*sequence, command.right_clip_id) != nullptr) {
              return error(EditErrorCode::DuplicateId, "right split clip id is already in use");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            if (location->track->locked) {
              return error(EditErrorCode::TrackLocked, "cannot split a clip on a locked track");
            }
            Clip left;
            Clip right;
            if (auto issue = splitClipObject(*location->clip, command.split_time,
                                             command.right_clip_id, left, right)) {
              return issue;
            }
            *location->clip = std::move(left);
            location->track->clips.push_back(std::move(right));
            sortClips(*location->track);
            return std::nullopt;
          },
          [&](const RemoveClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            const auto primary = *location->clip;
            const auto selected_ids = linkedClipIds(*sequence, primary, command.include_linked);
            const auto selected = idSet(selected_ids);
            std::unordered_set<EntityId> touched_tracks;
            for (const auto id : selected_ids) {
              auto selected_location = mutableClip(*sequence, id);
              if (!selected_location) {
                return error(EditErrorCode::EntityNotFound, "linked clip was not found");
              }
              if (selected_location->track->locked) {
                return error(EditErrorCode::TrackLocked,
                             "cannot remove a clip from a locked track");
              }
              if (command.ripple && command.include_linked &&
                  selected_location->clip->timeline_range != primary.timeline_range) {
                return error(EditErrorCode::InvalidArgument,
                             "linked ripple removal requires matching companion ranges");
              }
              touched_tracks.insert(selected_location->track->id);
            }
            for (auto& track : sequence->tracks) {
              if (!touched_tracks.contains(track.id)) {
                continue;
              }
              std::erase_if(track.clips,
                            [&](const Clip& clip) { return selected.contains(clip.id); });
              if (command.ripple) {
                for (auto& clip : track.clips) {
                  if (clip.timeline_range.start >= primary.timeline_range.end()) {
                    clip.timeline_range.start =
                        clip.timeline_range.start - primary.timeline_range.duration;
                  }
                }
                sortClips(track);
              }
            }
            return std::nullopt;
          },
          [&](const RollEditCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto left_location = mutableClip(*sequence, command.left_clip_id);
            auto right_location = mutableClip(*sequence, command.right_clip_id);
            if (!left_location || !right_location) {
              return error(EditErrorCode::EntityNotFound, "roll edit clip was not found");
            }
            if (left_location->track != right_location->track) {
              return error(EditErrorCode::InvalidArgument,
                           "roll edit clips must be on the same track");
            }
            auto& track = *left_location->track;
            if (track.locked) {
              return error(EditErrorCode::TrackLocked, "cannot roll clips on a locked track");
            }
            const auto left_index =
                static_cast<std::size_t>(std::distance(track.clips.begin(), left_location->clip));
            const auto right_index =
                static_cast<std::size_t>(std::distance(track.clips.begin(), right_location->clip));
            const auto left = *left_location->clip;
            const auto right = *right_location->clip;
            if (right_index != left_index + 1 ||
                left.timeline_range.end() != right.timeline_range.start) {
              return error(EditErrorCode::InvalidArgument,
                           "roll edit requires adjacent clips with a shared cut");
            }
            if (command.new_cut_time <= left.timeline_range.start ||
                command.new_cut_time >= right.timeline_range.end()) {
              return error(EditErrorCode::InvalidArgument, "roll edit would make a clip empty");
            }

            Clip rolled_left;
            Clip rolled_right;
            if (auto issue = trimPreservingMapping(
                    left,
                    TimeRange(left.timeline_range.start,
                              command.new_cut_time - left.timeline_range.start),
                    rolled_left)) {
              return issue;
            }
            if (auto issue = trimPreservingMapping(
                    right,
                    TimeRange(command.new_cut_time,
                              right.timeline_range.end() - command.new_cut_time),
                    rolled_right)) {
              return issue;
            }
            if (const auto issue = validateClip(project, track, rolled_left)) {
              return issue;
            }
            if (const auto issue = validateClip(project, track, rolled_right)) {
              return issue;
            }
            track.clips[left_index] = std::move(rolled_left);
            track.clips[right_index] = std::move(rolled_right);
            return std::nullopt;
          },
          [&](const SlipClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto primary_location = mutableClip(*sequence, command.clip_id);
            if (!primary_location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            const auto primary = *primary_location->clip;
            const auto source_delta = command.new_source_start - primary.source_range.start;
            const auto selected_ids = linkedClipIds(*sequence, primary, command.include_linked);
            std::vector<PlannedClip> plans;
            plans.reserve(selected_ids.size());
            for (const auto id : selected_ids) {
              auto selected_location = mutableClip(*sequence, id);
              if (!selected_location) {
                return error(EditErrorCode::EntityNotFound, "linked clip was not found");
              }
              if (selected_location->track->locked) {
                return error(EditErrorCode::TrackLocked, "cannot slip a clip on a locked track");
              }
              auto slipped = *selected_location->clip;
              slipped.source_range.start = slipped.source_range.start + source_delta;
              if (const auto issue = validateClip(project, *selected_location->track, slipped)) {
                return issue;
              }
              plans.push_back(PlannedClip{id, selected_location->track->id, std::move(slipped)});
            }
            for (auto& plan : plans) {
              auto selected_location = mutableClip(*sequence, plan.id);
              *selected_location->clip = std::move(plan.clip);
            }
            return std::nullopt;
          },
          [&](const SlideClipCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            auto& track = *location->track;
            if (track.locked) {
              return error(EditErrorCode::TrackLocked, "cannot slide a clip on a locked track");
            }
            const auto index =
                static_cast<std::size_t>(std::distance(track.clips.begin(), location->clip));
            if (index == 0 || index + 1 >= track.clips.size()) {
              return error(EditErrorCode::InvalidArgument,
                           "slide edit requires clips on both sides");
            }
            const auto previous = track.clips[index - 1];
            const auto selected_clip = track.clips[index];
            const auto next = track.clips[index + 1];
            if (previous.timeline_range.end() != selected_clip.timeline_range.start ||
                selected_clip.timeline_range.end() != next.timeline_range.start) {
              return error(EditErrorCode::InvalidArgument,
                           "slide edit requires three contiguous clips");
            }
            const auto new_end = command.new_start + selected_clip.timeline_range.duration;
            if (command.new_start <= previous.timeline_range.start ||
                new_end >= next.timeline_range.end()) {
              return error(EditErrorCode::InvalidArgument,
                           "slide edit exceeds neighboring source handles");
            }

            Clip trimmed_previous;
            Clip trimmed_next;
            if (auto issue = trimPreservingMapping(
                    previous,
                    TimeRange(previous.timeline_range.start,
                              command.new_start - previous.timeline_range.start),
                    trimmed_previous)) {
              return issue;
            }
            if (auto issue = trimPreservingMapping(
                    next, TimeRange(new_end, next.timeline_range.end() - new_end), trimmed_next)) {
              return issue;
            }
            auto moved = selected_clip;
            moved.timeline_range.start = command.new_start;
            if (const auto issue = validateClip(project, track, trimmed_previous)) {
              return issue;
            }
            if (const auto issue = validateClip(project, track, moved)) {
              return issue;
            }
            if (const auto issue = validateClip(project, track, trimmed_next)) {
              return issue;
            }
            track.clips[index - 1] = std::move(trimmed_previous);
            track.clips[index] = std::move(moved);
            track.clips[index + 1] = std::move(trimmed_next);
            return std::nullopt;
          },
          [&](const AddMarkerCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            if (std::any_of(sequence->markers.begin(), sequence->markers.end(),
                            [&](const Marker& marker) { return marker.id == command.marker.id; })) {
              return error(EditErrorCode::DuplicateId, "a marker with the same id already exists");
            }
            sequence->markers.push_back(command.marker);
            return std::nullopt;
          },
          [&](const UpdateMarkerCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            const auto found =
                std::find_if(sequence->markers.begin(), sequence->markers.end(),
                             [&](const Marker& marker) { return marker.id == command.marker.id; });
            if (found == sequence->markers.end()) {
              return error(EditErrorCode::EntityNotFound, "marker was not found");
            }
            *found = command.marker;
            return std::nullopt;
          },
          [&](const RemoveMarkerCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            const auto found =
                std::find_if(sequence->markers.begin(), sequence->markers.end(),
                             [&](const Marker& marker) { return marker.id == command.marker_id; });
            if (found == sequence->markers.end()) {
              return error(EditErrorCode::EntityNotFound, "marker was not found");
            }
            sequence->markers.erase(found);
            return std::nullopt;
          },
          [&](const AddCaptionCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            if (std::any_of(
                    sequence->captions.begin(), sequence->captions.end(),
                    [&](const Caption& caption) { return caption.id == command.caption.id; })) {
              return error(EditErrorCode::DuplicateId, "a caption with the same id already exists");
            }
            sequence->captions.push_back(command.caption);
            return std::nullopt;
          },
          [&](const UpdateCaptionCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            const auto found = std::find_if(
                sequence->captions.begin(), sequence->captions.end(),
                [&](const Caption& caption) { return caption.id == command.caption.id; });
            if (found == sequence->captions.end()) {
              return error(EditErrorCode::EntityNotFound, "caption was not found");
            }
            *found = command.caption;
            return std::nullopt;
          },
          [&](const RemoveCaptionCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            const auto found = std::find_if(
                sequence->captions.begin(), sequence->captions.end(),
                [&](const Caption& caption) { return caption.id == command.caption_id; });
            if (found == sequence->captions.end()) {
              return error(EditErrorCode::EntityNotFound, "caption was not found");
            }
            sequence->captions.erase(found);
            return std::nullopt;
          },
          [&](const AddClipEffectCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            if (location->track->locked) {
              return error(EditErrorCode::TrackLocked, "cannot edit effects on a locked track");
            }
            if (std::any_of(location->clip->effects.begin(), location->clip->effects.end(),
                            [&](const Effect& effect) { return effect.id == command.effect.id; })) {
              return error(EditErrorCode::DuplicateId, "an effect with the same id already exists");
            }
            location->clip->effects.push_back(command.effect);
            return std::nullopt;
          },
          [&](const RemoveClipEffectCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            if (location->track->locked) {
              return error(EditErrorCode::TrackLocked, "cannot edit effects on a locked track");
            }
            const auto found =
                std::find_if(location->clip->effects.begin(), location->clip->effects.end(),
                             [&](const Effect& effect) { return effect.id == command.effect_id; });
            if (found == location->clip->effects.end()) {
              return error(EditErrorCode::EntityNotFound, "effect was not found");
            }
            location->clip->effects.erase(found);
            return std::nullopt;
          },
          [&](const SetClipEffectParameterCommand& command) -> std::optional<EditError> {
            auto* sequence = mutableSequence(project, command.sequence_id);
            if (sequence == nullptr) {
              return error(EditErrorCode::EntityNotFound, "sequence was not found");
            }
            auto location = mutableClip(*sequence, command.clip_id);
            if (!location) {
              return error(EditErrorCode::EntityNotFound, "clip was not found");
            }
            if (location->track->locked) {
              return error(EditErrorCode::TrackLocked, "cannot edit effects on a locked track");
            }
            const auto found =
                std::find_if(location->clip->effects.begin(), location->clip->effects.end(),
                             [&](const Effect& effect) { return effect.id == command.effect_id; });
            if (found == location->clip->effects.end()) {
              return error(EditErrorCode::EntityNotFound, "effect was not found");
            }
            if (command.parameter.id.empty()) {
              return error(EditErrorCode::InvalidArgument, "effect parameter id cannot be empty");
            }
            found->parameters.insert_or_assign(command.parameter.id, command.parameter);
            return std::nullopt;
          }},
      op);
}

} // namespace

std::string commandName(const EditCommand& command) {
  return std::visit(
      [](const auto& operation) -> std::string {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, AddAssetCommand>)
          return "Add asset";
        if constexpr (std::is_same_v<T, RemoveAssetCommand>)
          return "Remove asset";
        if constexpr (std::is_same_v<T, AddSequenceCommand>)
          return "Add sequence";
        if constexpr (std::is_same_v<T, RemoveSequenceCommand>)
          return "Remove sequence";
        if constexpr (std::is_same_v<T, SetSequenceFormatCommand>)
          return "Set sequence format";
        if constexpr (std::is_same_v<T, AddTrackCommand>)
          return "Add track";
        if constexpr (std::is_same_v<T, RemoveTrackCommand>)
          return "Remove track";
        if constexpr (std::is_same_v<T, InsertClipCommand>)
          return "Insert clip";
        if constexpr (std::is_same_v<T, MoveClipCommand>)
          return "Move clip";
        if constexpr (std::is_same_v<T, TrimClipCommand>)
          return "Trim clip";
        if constexpr (std::is_same_v<T, SplitClipCommand>)
          return "Split clip";
        if constexpr (std::is_same_v<T, RemoveClipCommand>)
          return "Remove clip";
        if constexpr (std::is_same_v<T, RollEditCommand>)
          return "Roll edit";
        if constexpr (std::is_same_v<T, SlipClipCommand>)
          return "Slip clip";
        if constexpr (std::is_same_v<T, SlideClipCommand>)
          return "Slide clip";
        if constexpr (std::is_same_v<T, AddMarkerCommand>)
          return "Add marker";
        if constexpr (std::is_same_v<T, UpdateMarkerCommand>)
          return "Update marker";
        if constexpr (std::is_same_v<T, RemoveMarkerCommand>)
          return "Remove marker";
        if constexpr (std::is_same_v<T, AddCaptionCommand>)
          return "Add caption";
        if constexpr (std::is_same_v<T, UpdateCaptionCommand>)
          return "Update caption";
        if constexpr (std::is_same_v<T, RemoveCaptionCommand>)
          return "Remove caption";
        if constexpr (std::is_same_v<T, AddClipEffectCommand>)
          return "Add clip effect";
        if constexpr (std::is_same_v<T, RemoveClipEffectCommand>)
          return "Remove clip effect";
        if constexpr (std::is_same_v<T, SetClipEffectParameterCommand>) {
          return "Set clip effect parameter";
        }
        return "Edit";
      },
      command.operation);
}

TimelineSnapshot::TimelineSnapshot(Revision revision, std::shared_ptr<const Project> project,
                                   EntityId sequence_id)
    : revision_(revision), project_(std::move(project)), sequence_id_(sequence_id) {}

const Project& TimelineSnapshot::project() const {
  if (!project_) {
    throw std::logic_error("empty timeline snapshot");
  }
  return *project_;
}

const Sequence& TimelineSnapshot::sequence() const {
  const auto* result = findSequence(project(), sequence_id_);
  if (result == nullptr) {
    throw std::logic_error("snapshot sequence no longer exists");
  }
  return *result;
}

const Track* TimelineSnapshot::findTrack(EntityId id) const noexcept {
  if (!project_) {
    return nullptr;
  }
  const auto* current_sequence = findSequence(*project_, sequence_id_);
  return current_sequence == nullptr ? nullptr
                                     : video_editor::edit::findTrack(*current_sequence, id);
}

const Clip* TimelineSnapshot::findClip(EntityId id) const noexcept {
  if (!project_) {
    return nullptr;
  }
  const auto* current_sequence = findSequence(*project_, sequence_id_);
  return current_sequence == nullptr ? nullptr
                                     : video_editor::edit::findClip(*current_sequence, id);
}

Time TimelineSnapshot::duration() const {
  return sequenceDuration(sequence());
}

std::vector<Gap> TimelineSnapshot::gaps(EntityId track_id,
                                        std::optional<Time> requested_end) const {
  const auto* track = findTrack(track_id);
  if (track == nullptr) {
    return {};
  }
  const auto limit = requested_end.value_or(duration());
  if (limit.isNegative()) {
    throw std::invalid_argument("gap range end cannot be negative");
  }
  std::vector<Gap> result;
  auto cursor = Time{};
  for (const auto& clip : track->clips) {
    if (clip.timeline_range.start >= limit) {
      break;
    }
    if (clip.timeline_range.start > cursor) {
      result.push_back(Gap{TimeRange(cursor, std::min(clip.timeline_range.start, limit) - cursor)});
    }
    cursor = std::max(cursor, clip.timeline_range.end());
    if (cursor >= limit) {
      return result;
    }
  }
  if (cursor < limit) {
    result.push_back(Gap{TimeRange(cursor, limit - cursor)});
  }
  return result;
}

struct TimelineEditor::HistoryEntry final {
  EditCommand last_command;
  std::string coalescing_key;
  std::size_t command_count{1};
  std::shared_ptr<const Project> before;
  std::shared_ptr<const Project> after;
};

TimelineEditor::~TimelineEditor() = default;

TimelineEditor::TimelineEditor(Project initial_project) {
  for (auto& sequence : initial_project.sequences) {
    for (auto& track : sequence.tracks) {
      sortClips(track);
    }
  }
  if (const auto issue = validateProject(initial_project)) {
    throw std::invalid_argument(issue->message);
  }
  state_ = std::make_shared<const Project>(std::move(initial_project));
  revisions_.emplace(0, state_);
}

Revision TimelineEditor::revision() const noexcept {
  std::shared_lock lock(mutex_);
  return revision_;
}

Result<Revision, EditError> TimelineEditor::staleRevision(Revision expected) const {
  return Result<Revision, EditError>::failure(EditError{
      EditErrorCode::RevisionConflict,
      "edit was based on a stale project revision",
      expected,
      revision_,
  });
}

Result<Revision, EditError> TimelineEditor::commitState(std::shared_ptr<const Project> next_state) {
  if (revision_.value == std::numeric_limits<std::uint64_t>::max()) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::ArithmeticOverflow, "project revision counter overflowed"));
  }
  revision_.value += 1;
  state_ = std::move(next_state);
  revisions_.insert_or_assign(revision_.value, state_);
  return Result<Revision, EditError>::success(revision_);
}

Result<Revision, EditError> TimelineEditor::apply(EditCommand command, Revision expected_revision) {
  std::unique_lock lock(mutex_);
  if (expected_revision != revision_) {
    return staleRevision(expected_revision);
  }

  try {
    auto candidate = std::make_shared<Project>(*state_);
    if (auto issue = applyOperation(*candidate, command.operation)) {
      return Result<Revision, EditError>::failure(std::move(*issue));
    }
    for (auto& sequence : candidate->sequences) {
      for (auto& track : sequence.tracks) {
        sortClips(track);
      }
    }
    if (auto issue = validateProject(*candidate)) {
      return Result<Revision, EditError>::failure(std::move(*issue));
    }

    const auto before = state_;
    const std::shared_ptr<const Project> after = std::move(candidate);
    if (revision_.value == std::numeric_limits<std::uint64_t>::max()) {
      return Result<Revision, EditError>::failure(
          error(EditErrorCode::ArithmeticOverflow, "project revision counter overflowed"));
    }
    if (history_cursor_ < history_.size()) {
      history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_),
                     history_.end());
    }

    const bool can_coalesce = !command.coalescing_key.empty() && !history_.empty() &&
                              history_cursor_ == history_.size() &&
                              history_.back().coalescing_key == command.coalescing_key;
    if (can_coalesce) {
      auto& entry = history_.back();
      entry.after = after;
      entry.last_command = std::move(command);
      entry.command_count += 1;
    } else {
      history_.push_back(HistoryEntry{command, command.coalescing_key, 1, before, after});
      history_cursor_ += 1;
    }
    return commitState(after);
  } catch (const std::overflow_error& exception) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::ArithmeticOverflow, exception.what()));
  } catch (const std::invalid_argument& exception) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::InvalidArgument, exception.what()));
  }
}

Result<Revision, EditError> TimelineEditor::undo(Revision expected_revision) {
  std::unique_lock lock(mutex_);
  if (expected_revision != revision_) {
    return staleRevision(expected_revision);
  }
  if (history_cursor_ == 0) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::NothingToUndo, "there is no edit to undo"));
  }
  if (revision_.value == std::numeric_limits<std::uint64_t>::max()) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::ArithmeticOverflow, "project revision counter overflowed"));
  }
  const auto next_state = history_[history_cursor_ - 1].before;
  --history_cursor_;
  return commitState(next_state);
}

Result<Revision, EditError> TimelineEditor::redo(Revision expected_revision) {
  std::unique_lock lock(mutex_);
  if (expected_revision != revision_) {
    return staleRevision(expected_revision);
  }
  if (history_cursor_ >= history_.size()) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::NothingToRedo, "there is no edit to redo"));
  }
  if (revision_.value == std::numeric_limits<std::uint64_t>::max()) {
    return Result<Revision, EditError>::failure(
        error(EditErrorCode::ArithmeticOverflow, "project revision counter overflowed"));
  }
  const auto next_state = history_[history_cursor_].after;
  ++history_cursor_;
  return commitState(next_state);
}

bool TimelineEditor::canUndo() const noexcept {
  std::shared_lock lock(mutex_);
  return history_cursor_ > 0;
}

bool TimelineEditor::canRedo() const noexcept {
  std::shared_lock lock(mutex_);
  return history_cursor_ < history_.size();
}

std::vector<HistoryEntryView> TimelineEditor::history() const {
  std::shared_lock lock(mutex_);
  std::vector<HistoryEntryView> result;
  result.reserve(history_.size());
  for (const auto& entry : history_) {
    result.push_back(HistoryEntryView{commandName(entry.last_command), entry.coalescing_key,
                                      entry.command_count});
  }
  return result;
}

Result<TimelineSnapshot, EditError> TimelineEditor::snapshot(EntityId sequence_id,
                                                             Revision requested_revision) const {
  std::shared_lock lock(mutex_);
  const auto state = revisions_.find(requested_revision.value);
  if (state == revisions_.end()) {
    return Result<TimelineSnapshot, EditError>::failure(
        error(EditErrorCode::RevisionNotFound, "requested project revision is not retained"));
  }
  if (findSequence(*state->second, sequence_id) == nullptr) {
    return Result<TimelineSnapshot, EditError>::failure(
        error(EditErrorCode::EntityNotFound, "sequence was not found"));
  }
  return Result<TimelineSnapshot, EditError>::success(
      TimelineSnapshot(requested_revision, state->second, sequence_id));
}

std::shared_ptr<const Project> TimelineEditor::projectAt(Revision requested_revision) const {
  std::shared_lock lock(mutex_);
  const auto found = revisions_.find(requested_revision.value);
  return found == revisions_.end() ? nullptr : found->second;
}

} // namespace video_editor::edit

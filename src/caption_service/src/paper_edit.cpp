// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "internal.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] bool trackAcceptsKind(const edit::TrackKind track, const edit::ClipKind clip) {
  switch (track) {
    case edit::TrackKind::Video:
      return clip == edit::ClipKind::Video || clip == edit::ClipKind::Title;
    case edit::TrackKind::Audio:
      return clip == edit::ClipKind::Audio;
    case edit::TrackKind::Caption:
      return false;
  }
  return false;
}

[[nodiscard]] PaperEditError makeError(const PaperEditErrorCode code, std::string message,
                                       const std::size_t index = 0) {
  return PaperEditError{code, std::move(message), index};
}

} // namespace

PaperEditResult buildPaperEditAssembly(const edit::TimelineSnapshot& snapshot,
                                       const edit::EntityId target_track_id,
                                       std::span<const PaperEditSelection> selections,
                                       const PaperEditOptions& options) {
  if (selections.empty()) {
    return PaperEditResult::failure(
        makeError(PaperEditErrorCode::NoSelections, "at least one passage is required"));
  }
  if (options.assembly_start.isNegative() || options.gap_between.isNegative()) {
    return PaperEditResult::failure(makeError(
        PaperEditErrorCode::InvalidRange, "assembly start and gap must be non-negative"));
  }

  const edit::Track* target = snapshot.findTrack(target_track_id);
  if (target == nullptr) {
    return PaperEditResult::failure(
        makeError(PaperEditErrorCode::TargetTrackNotFound, "target track was not found"));
  }
  if (target->kind == edit::TrackKind::Caption) {
    return PaperEditResult::failure(makeError(PaperEditErrorCode::IncompatibleTrackKind,
                                              "caption tracks cannot hold assembled passages"));
  }
  if (target->locked) {
    return PaperEditResult::failure(
        makeError(PaperEditErrorCode::TargetTrackLocked, "target track is locked"));
  }

  PaperEditAssembly assembly;
  assembly.base_revision = snapshot.revision();
  assembly.target_track_id = target_track_id;
  assembly.clips.reserve(selections.size());

  edit::Time offset = options.assembly_start;
  for (std::size_t index = 0; index < selections.size(); ++index) {
    const auto& selection = selections[index];
    if (selection.timeline_range.duration <= edit::Time{} ||
        selection.timeline_range.start.isNegative()) {
      return PaperEditResult::failure(makeError(PaperEditErrorCode::InvalidRange,
                                                "passage range must be positive", index));
    }
    const edit::Clip* source = snapshot.findClip(selection.source_clip_id);
    if (source == nullptr) {
      return PaperEditResult::failure(
          makeError(PaperEditErrorCode::SourceClipNotFound, "source clip was not found", index));
    }
    if (selection.timeline_range.start < source->timeline_range.start ||
        selection.timeline_range.end() > source->timeline_range.end()) {
      return PaperEditResult::failure(makeError(
          PaperEditErrorCode::RangeOutsideClip, "passage is outside its source clip", index));
    }
    if (!trackAcceptsKind(target->kind, source->kind)) {
      return PaperEditResult::failure(makeError(PaperEditErrorCode::IncompatibleTrackKind,
                                                "passage clip kind is incompatible with the "
                                                "target track",
                                                index));
    }

    edit::Clip fragment = *source;
    fragment.id = detail::fragmentId(selection.source_clip_id, index);
    fragment.source_range = detail::sourceRangeForSubClip(*source, selection.timeline_range.start,
                                                          selection.timeline_range.end());
    fragment.timeline_range = edit::TimeRange(offset, selection.timeline_range.duration);
    // Assembled passages stand alone; drop any linked-group reference whose
    // companions are not part of this rough cut.
    fragment.linked_group = std::nullopt;

    assembly.review_items.push_back(
        {fragment.timeline_range,
         source->name.empty() ? std::string{"Assemble passage"}
                              : "Assemble passage from '" + source->name + "'"});
    assembly.clips.push_back(std::move(fragment));

    offset = assembly.clips.back().timeline_range.end() + options.gap_between;
  }

  edit::TrackClipReplacement replacement;
  replacement.track_id = target_track_id;
  replacement.kind = target->kind;
  replacement.clips = assembly.clips;
  assembly.timeline_change =
      edit::ApplyTimelineCutChangeSetCommand{snapshot.sequence().id, {std::move(replacement)}};

  return PaperEditResult::success(std::move(assembly));
}

} // namespace video_editor::caption_service

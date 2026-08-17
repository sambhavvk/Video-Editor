// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace video_editor::edit {

enum class InsertMode { RejectOverlap, Overwrite, Ripple };

struct AddAssetCommand final {
  Asset asset;
};
struct RemoveAssetCommand final {
  EntityId asset_id;
};
struct AddSequenceCommand final {
  Sequence sequence;
};
struct RemoveSequenceCommand final {
  EntityId sequence_id;
};
struct SetSequenceFormatCommand final {
  EntityId sequence_id;
  Rate frame_rate{30, 1};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
};
struct AddTrackCommand final {
  EntityId sequence_id;
  Track track;
  std::optional<std::size_t> index;
};
struct RemoveTrackCommand final {
  EntityId sequence_id;
  EntityId track_id;
};
struct RenameTrackCommand final {
  EntityId sequence_id;
  EntityId track_id;
  std::string name;
};
struct ReorderTrackCommand final {
  EntityId sequence_id;
  EntityId track_id;
  std::size_t index{0};
};
struct SetTrackLockedCommand final {
  EntityId sequence_id;
  EntityId track_id;
  bool locked{false};
};
struct SetTrackVisibilityCommand final {
  EntityId sequence_id;
  EntityId track_id;
  bool visible{true};
};
struct SetTrackTargetedCommand final {
  EntityId sequence_id;
  EntityId track_id;
  bool targeted{true};
};
struct InsertClipCommand final {
  EntityId sequence_id;
  EntityId track_id;
  Clip clip;
  InsertMode mode{InsertMode::RejectOverlap};
};
struct MoveClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  EntityId destination_track_id;
  Time new_start{};
  InsertMode mode{InsertMode::RejectOverlap};
  // When enabled, every clip with the same linked_group moves by the same
  // exact timeline delta. The selected clip alone may change tracks; linked
  // companions remain on their current tracks.
  bool include_linked{false};
};
struct TrimClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  TimeRange timeline_range;
  TimeRange source_range;
  // Linked companions receive the same head and tail timeline deltas. Their
  // source ranges are derived from their own rate and direction.
  bool include_linked{false};
  InsertMode mode{InsertMode::RejectOverlap};
};
struct LinkedSplitId final {
  EntityId clip_id;
  EntityId right_clip_id;
};
struct SplitClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Time split_time;
  EntityId right_clip_id{EntityId::generate()};
  bool include_linked{false};
  // Required exactly once for every linked companion when include_linked is
  // true. The selected clip continues to use right_clip_id.
  std::vector<LinkedSplitId> linked_right_clip_ids;
};
struct RemoveClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  bool ripple{false};
  bool include_linked{false};
};
struct CloseGapCommand final {
  EntityId sequence_id;
  EntityId track_id;
  TimeRange gap;
};
struct RollEditCommand final {
  EntityId sequence_id;
  EntityId left_clip_id;
  EntityId right_clip_id;
  Time new_cut_time{};
};
struct SlipClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Time new_source_start{};
  bool include_linked{false};
};
struct SlideClipCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Time new_start{};
};
struct AddMarkerCommand final {
  EntityId sequence_id;
  Marker marker;
};
struct UpdateMarkerCommand final {
  EntityId sequence_id;
  Marker marker;
};
struct RemoveMarkerCommand final {
  EntityId sequence_id;
  EntityId marker_id;
};
struct AddCaptionCommand final {
  EntityId sequence_id;
  Caption caption;
};
struct UpdateCaptionCommand final {
  EntityId sequence_id;
  Caption caption;
};
struct RemoveCaptionCommand final {
  EntityId sequence_id;
  EntityId caption_id;
};
struct ApplyCaptionChangeSetCommand final {
  EntityId sequence_id;
  std::vector<Caption> added;
  std::vector<Caption> updated;
  std::vector<EntityId> removed;
};

struct TrackClipReplacement final {
  EntityId track_id;
  TrackKind kind{TrackKind::Video};
  std::vector<Clip> clips;
};

// The replacement list is complete for every listed track. Proposal planners
// materialize all fragment IDs before this command is submitted.
struct ApplyTimelineCutChangeSetCommand final {
  EntityId sequence_id;
  std::vector<TrackClipReplacement> tracks;
};
struct AddClipEffectCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Effect effect;
};
struct RemoveClipEffectCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  EntityId effect_id;
};
struct SetClipEffectParameterCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  EntityId effect_id;
  EffectParameter parameter;
};
struct SetClipTransformCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Transform transform;
};
struct SetClipBlendModeCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  BlendMode blend_mode{BlendMode::Normal};
};
struct SetClipAudioPropertiesCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  double gain_db{0.0};
  double pan{0.0};
  Time fade_in{};
  Time fade_out{};
};
struct SetClipTitleCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Title title;
};
struct SetClipSpeedCommand final {
  EntityId sequence_id;
  EntityId clip_id;
  Rate playback_rate{1, 1};
  bool reversed{false};
};
struct SetTrackAudioStateCommand final {
  EntityId sequence_id;
  EntityId track_id;
  bool muted{false};
  bool solo{false};
};
struct SetTrackAudioMixCommand final {
  EntityId sequence_id;
  EntityId track_id;
  double gain_db{0.0};
  double pan{0.0};
};
struct AddTrackEffectCommand final {
  EntityId sequence_id;
  EntityId track_id;
  Effect effect;
};
struct RemoveTrackEffectCommand final {
  EntityId sequence_id;
  EntityId track_id;
  EntityId effect_id;
};
struct SetTrackEffectParameterCommand final {
  EntityId sequence_id;
  EntityId track_id;
  EntityId effect_id;
  EffectParameter parameter;
};
struct AddTransitionCommand final {
  EntityId sequence_id;
  Transition transition;
};
struct UpdateTransitionCommand final {
  EntityId sequence_id;
  Transition transition;
};
struct RemoveTransitionCommand final {
  EntityId sequence_id;
  EntityId transition_id;
};

using EditOperation = std::variant<
    AddAssetCommand, RemoveAssetCommand, AddSequenceCommand, RemoveSequenceCommand, AddTrackCommand,
    RemoveTrackCommand, InsertClipCommand, MoveClipCommand, TrimClipCommand, SplitClipCommand,
    RemoveClipCommand, RollEditCommand, SlipClipCommand, SlideClipCommand, AddMarkerCommand,
    UpdateMarkerCommand, RemoveMarkerCommand, AddCaptionCommand, UpdateCaptionCommand,
    RemoveCaptionCommand, AddClipEffectCommand, RemoveClipEffectCommand,
    SetClipEffectParameterCommand, SetSequenceFormatCommand, SetClipTransformCommand,
    SetClipBlendModeCommand, SetClipAudioPropertiesCommand, SetTrackAudioStateCommand,
    SetClipTitleCommand, SetClipSpeedCommand, AddTransitionCommand, UpdateTransitionCommand,
    RemoveTransitionCommand, RenameTrackCommand, ReorderTrackCommand, SetTrackLockedCommand,
    SetTrackVisibilityCommand, SetTrackTargetedCommand, CloseGapCommand, SetTrackAudioMixCommand,
    AddTrackEffectCommand, RemoveTrackEffectCommand, SetTrackEffectParameterCommand,
    ApplyCaptionChangeSetCommand, ApplyTimelineCutChangeSetCommand>;

struct EditCommand final {
  EditOperation operation;
  // Commands with the same non-empty key collapse into one undo step while
  // they remain adjacent. A UI should use one unique key per gesture.
  std::string coalescing_key;
};

[[nodiscard]] std::string commandName(const EditCommand& command);

} // namespace video_editor::edit

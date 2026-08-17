<!-- SPDX-License-Identifier: MPL-2.0 -->

# Edit command API reference

Header: `video_editor/edit_model/commands.h`

Namespace: `video_editor::edit`

## Overview

This header defines the owned value types accepted by `TimelineEditor`. Every mutation is applied
with an expected revision. Validation failure leaves project state and history unchanged; related
operations can be submitted through `TimelineEditor::applyBatch` for one atomic revision.

## Enumerations

### `InsertMode`

| Value | Description |
| --- | --- |
| `RejectOverlap` | Reject a candidate that overlaps unaffected material. This is the default. |
| `Overwrite` | Keep timeline positions fixed while removing or edge-trimming covered material. |
| `Ripple` | Move following material by the exact duration change. |

## Asset, sequence, and track commands

| Type | Fields | Contract |
| --- | --- | --- |
| `AddAssetCommand` | `asset` | Adds one validated asset with a project-unique ID. |
| `RemoveAssetCommand` | `asset_id` | Removes an unused asset; references make the command fail. |
| `AddSequenceCommand` | `sequence` | Adds a complete validated sequence. |
| `RemoveSequenceCommand` | `sequence_id` | Removes the selected sequence. |
| `SetSequenceFormatCommand` | `sequence_id`, `frame_rate`, `width`, `height` | Replaces the exact frame rate and raster dimensions. |
| `AddTrackCommand` | `sequence_id`, `track`, optional `index` | Inserts a complete track at the requested position or appends it. |
| `RemoveTrackCommand` | `sequence_id`, `track_id` | Removes an unlocked track when doing so leaves a valid project. |
| `RenameTrackCommand` | `sequence_id`, `track_id`, `name` | Replaces an unlocked track's bounded nonempty UTF-8 name. |
| `ReorderTrackCommand` | `sequence_id`, `track_id`, `index` | Moves an unlocked track to an existing zero-based sequence index. |
| `SetTrackLockedCommand` | `sequence_id`, `track_id`, `locked` | Changes the editorial lock; unlocking the track itself is always possible. |
| `SetTrackVisibilityCommand` | `sequence_id`, `track_id`, `visible` | Enables or disables visual render contribution. Locking does not block this presentation state. |
| `SetTrackTargetedCommand` | `sequence_id`, `track_id`, `targeted` | Changes the insertion-routing hint. It has no render effect. |
| `SetTrackAudioStateCommand` | `sequence_id`, `track_id`, `muted`, `solo` | Replaces mute/solo on an audio track; the live mixer state may change while locked. |
| `SetTrackAudioMixCommand` | `sequence_id`, `track_id`, `gain_db`, `pan` | Replaces validated audio-track gain (−96…+24 dB) and pan (−1…+1). |
| `AddTrackEffectCommand` | `sequence_id`, `track_id`, `effect` | Appends a validated typed/versioned effect to an unlocked audio track. |
| `RemoveTrackEffectCommand` | `sequence_id`, `track_id`, `effect_id` | Removes one effect from an unlocked audio track. |
| `SetTrackEffectParameterCommand` | `sequence_id`, `track_id`, `effect_id`, `parameter` | Replaces one validated track-effect parameter. |

## Clip and precision commands

| Type | Fields | Contract |
| --- | --- | --- |
| `InsertClipCommand` | `sequence_id`, `track_id`, `clip`, `mode` | Inserts a compatible clip using the selected overlap policy. |
| `MoveClipCommand` | `sequence_id`, `clip_id`, `destination_track_id`, `new_start`, `mode`, `include_linked` | Moves the clip exactly; linked companions receive the same delta and remain on their tracks. |
| `TrimClipCommand` | `sequence_id`, `clip_id`, `timeline_range`, `source_range`, `include_linked`, `mode` | Replaces exact visible/source ranges and optionally applies linked and ripple/overwrite policy. |
| `LinkedSplitId` | `clip_id`, `right_clip_id` | Maps one linked companion to the explicit identity of its right half. |
| `SplitClipCommand` | `sequence_id`, `clip_id`, `split_time`, `right_clip_id`, `include_linked`, `linked_right_clip_ids` | Splits strictly inside every participant. Linked mode requires one complete companion-ID mapping. |
| `RemoveClipCommand` | `sequence_id`, `clip_id`, `ripple`, `include_linked` | Removes one clip or linked group and optionally closes each affected track. |
| `CloseGapCommand` | `sequence_id`, `track_id`, `gap` | Verifies an exact current derived gap and shifts later clips left; terminal/stale gaps fail. |
| `RollEditCommand` | `sequence_id`, `left_clip_id`, `right_clip_id`, `new_cut_time` | Moves one adjacent shared cut while retaining the pair's outer bounds. |
| `SlipClipCommand` | `sequence_id`, `clip_id`, `new_source_start`, `include_linked` | Changes source windows while retaining timeline positions. |
| `SlideClipCommand` | `sequence_id`, `clip_id`, `new_start` | Moves a middle clip while trimming its immediate neighbors and retaining the outer span. |

## Marker, caption, effect, and property commands

| Type | Fields | Contract |
| --- | --- | --- |
| `AddMarkerCommand` | `sequence_id`, `marker` | Adds one marker with a unique ID and valid range/color. |
| `UpdateMarkerCommand` | `sequence_id`, `marker` | Replaces the complete marker selected by its ID. |
| `RemoveMarkerCommand` | `sequence_id`, `marker_id` | Removes one marker. |
| `AddCaptionCommand` | `sequence_id`, `caption` | Adds a validated caption. |
| `UpdateCaptionCommand` | `sequence_id`, `caption` | Replaces the complete caption selected by its ID. |
| `RemoveCaptionCommand` | `sequence_id`, `caption_id` | Removes one caption. |
| `ApplyCaptionChangeSetCommand` | `sequence_id`, complete added/updated/removed values | Applies a nonempty deterministic caption change set atomically; duplicates, missing IDs, invalid cues, and no-ops reject the command. |
| `TrackClipReplacement` | track identity/kind plus complete clip list | Supplies one authoritative replacement list inside a timeline-cut proposal. |
| `ApplyTimelineCutChangeSetCommand` | `sequence_id`, complete affected-track replacements | Applies a nonempty review-approved cut set atomically; caption tracks, locked/missing/kind-mismatched tracks, invalid clips, and no-ops reject it. |
| `AddClipEffectCommand` | `sequence_id`, `clip_id`, `effect` | Appends a typed or opaque effect to an unlocked clip. |
| `RemoveClipEffectCommand` | `sequence_id`, `clip_id`, `effect_id` | Removes one clip effect. |
| `SetClipEffectParameterCommand` | `sequence_id`, `clip_id`, `effect_id`, `parameter` | Replaces one typed, versioned effect parameter. |
| `SetClipTransformCommand` | `sequence_id`, `clip_id`, `transform` | Replaces visual transform/crop/opacity on a video or title clip. |
| `SetClipBlendModeCommand` | `sequence_id`, `clip_id`, `blend_mode` | Replaces the video/title composition mode. |
| `SetClipAudioPropertiesCommand` | `sequence_id`, `clip_id`, `gain_db`, `pan`, `fade_in`, `fade_out` | Replaces validated audio clip properties. |
| `SetClipTitleCommand` | `sequence_id`, `clip_id`, `title` | Replaces the canonical payload of a title clip. |
| `AddTransitionCommand` | `sequence_id`, `transition` | Adds a validated sequence-owned transition. |
| `UpdateTransitionCommand` | `sequence_id`, `transition` | Replaces the transition selected by its ID. |
| `RemoveTransitionCommand` | `sequence_id`, `transition_id` | Removes one transition from unlocked participants. |

## Command container

`EditOperation` is the variant of every command type above. Existing alternatives retain their
order; new alternatives are appended so callers that inspect variant indices do not observe an
avoidable compatibility change.

`EditCommand` owns one `operation` and an optional `coalescing_key`. Adjacent successful single
commands with the same nonempty key collapse into one undo entry. Atomic batches retain their own
display name and command count.

## Functions

### `std::string commandName(const EditCommand& command)`

Returns the stable English history label for the contained operation. It does not mutate the
command and is safe for any valid `EditOperation` alternative.

## Usage example

```cpp
std::vector<edit::EditCommand> commands;
commands.push_back({edit::SetTrackVisibilityCommand{sequence_id, video_track_id, false}, {}});
commands.push_back({edit::SetTrackTargetedCommand{sequence_id, video_track_id, false}, {}});

auto result = editor.applyBatch(std::move(commands), editor.revision(), "Update track state");
```

Command values own their inputs and retain no caller references. The edit model serializes
mutation internally; immutable snapshots remain suitable for concurrent readers.

AI assistance has been used to create this output.

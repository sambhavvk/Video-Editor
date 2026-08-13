<!-- SPDX-License-Identifier: MPL-2.0 -->

# Exact timeline and edit semantics

This document describes the edit-model contract. The [status matrix](../beta-feature-status.md)
identifies which commands are exposed by the current desktop.

## Exact time

`Time` is a signed 64-bit value divided by a nonzero unsigned 32-bit timescale. Timeline decisions
do not use floating point. Comparison, addition, subtraction, scaling, and rescaling use 128-bit
intermediates and reject overflow. Callers choose one of five rounding policies: toward zero,
floor, ceiling, nearest with ties away, or nearest with ties to even.

`TimeRange` is half-open: `[start, start + duration)`. Touching ranges do not overlap. Clips and
captions require a non-negative start and positive duration; an asset duration may be zero. A
`Rate` is an exact positive numerator/denominator and converts frame counts to time or time to frame
counts with an explicit rounding mode.

Sequence defaults are 30000/1001 fps, 1920×1080, and 48 kHz. The first inserted video asset can
replace frame rate and dimensions through `SetSequenceFormatCommand`.

## Identity and revisions

Every entity has a non-nil UUIDv7 `EntityId`; IDs are unique across the whole project. Applying a
command requires the caller's `expected_revision`. If it does not equal the current head, the model
returns a revision conflict and makes no change.

A successful command validates a candidate project, publishes the next immutable revision, and
adds one undo record. `applyBatch` applies an ordered set of commands to one candidate and publishes
exactly one revision/history entry; an empty batch or any invalid member publishes nothing. Commands
with the same adjacent nonempty coalescing key collapse into one undo step. Undo and redo themselves
require the current revision and publish a new head revision.

## Track and overlap invariants

- Video tracks accept video and title clips; audio tracks accept audio clips.
- Caption tracks do not contain media clips; sequence captions are separate entities.
- A track's clips are sorted by start time and then stable entity identity.
- Clips on one track may touch but may not overlap after a completed command.
- Locked tracks reject mutations.
- Track names are bounded nonempty UTF-8. Rename, reorder, lock, output visibility, and targeting
  use typed revisioned commands. Visibility controls visual render contribution; targeting chooses
  deterministic compatible insertion destinations and is not a renderer input.
- Non-title clips reference an existing compatible asset, and their source window must remain
  inside that asset.
- Opacity is `[0, 1]`; pan is `[-1, 1]`.

Insert, move, and trim accept an `InsertMode` policy:

- `RejectOverlap` fails when the requested range intersects another clip.
- `Overwrite` removes or splits covered material so the inserted range wins while preserving
  unaffected portions.
- `Ripple` shifts material at and after the edit boundary by the inserted/moved duration according
  to the command's exact rules.

For trim, normal mode changes only the clip boundary. Ripple changes the boundary and moves later
material by the exact duration delta. Overwrite keeps later timeline positions fixed; extension
removes fully covered clips and edge-trims a final partial overlap, while shrink may leave a gap.

All commands and command batches are atomic: validation failure, a locked linked companion, an
overlap, or insufficient source handles leaves the project and revision unchanged.

## Split and trim mapping

A split time must lie strictly inside every participating clip. Timeline duration before the cut is scaled by the
clip's exact playback rate with nearest-ties-even rounding into the source timescale. Forward clips
divide source media from low to high; reversed clips preserve the displayed mapping while their
stored source ranges remain low to high. Linked split carries an explicit `clip_id` → right-half-ID
mapping for every companion, so every generated entity identity is command data and the complete
operation rejects missing, extra, duplicate, nil, locked, or unsplittable participants.

A trim changes timeline boundaries and source boundaries together so every unchanged timeline
point continues to show the same source point. It fails if the result is empty, starts before zero,
extends beyond the asset, or requires unavailable source handles. Linked trims apply the same head
and tail timeline deltas to companions, then calculate source deltas from each companion's own rate
and direction.

## Linked clips

Linked behavior is explicit rather than globally implicit. Move, trim, split, remove, and slip commands
contain `include_linked`; callers that leave it false mutate only the selected clip.

When enabled:

- all clips sharing the selected clip's `linked_group` participate;
- a move applies the same timeline delta to every companion, but only the selected clip may move to
  another track;
- trim derives each companion's source window independently;
- split cuts every aligned companion and consumes its explicit right-half identity;
- ripple removal closes the removed range independently on every affected track;
- any invalid or locked participant rejects the complete command.

The desktop enables linked behavior for pointer move, trim, split, and delete. Multi-selection
operations de-duplicate linked groups and use one atomic batch revision.

## Selection, markers, and gaps

Clip, marker, and gap selection is transient controller/UI state. It is not serialized, journaled,
or included in immutable snapshots. Multi-clip edits operate on stable entity IDs resolved against
the current expected revision.

Markers are canonical sequence entities changed by add, update, and remove commands. Gaps are
derived half-open ranges returned by a snapshot, not stored entities. `CloseGapCommand` names a
track and exact range, verifies that the same gap still exists and the track is unlocked, then
shifts later material left. A stale or partial gap range is rejected rather than closing different
empty space.

## Roll, slip, and slide

- **Roll** moves the shared cut between two adjacent clips without moving their outer timeline
  boundaries. The clips must form an exact adjacent pair and both need sufficient source handles.
- **Slip** keeps a clip's timeline range fixed and selects a new source start. Linked slip preserves
  linked timing according to each clip's source mapping.
- **Slide** moves a middle clip while changing the neighboring clips' touching boundaries so the
  overall three-clip span remains fixed. It requires the validated neighboring layout and handles.

These commands are exposed by the desktop precision tools. Their pointer previews are transient;
release produces one typed command or atomic batch and Escape produces no revision.

## Clip-property commands

Transform, blend, and audio properties use dedicated typed commands so Inspector changes receive
the same expected-revision, locking, validation, persistence, and undo behavior as structural edits.

- Position coordinates must be finite and within ±1,000,000 pixels. Scale components may be signed
  but their magnitudes must stay from 0.0001 through 1000. Rotation is bounded to ±36,000 degrees;
  normalized anchor and crop values must retain positive image area; opacity is `[0, 1]`.
- Supported blend modes are Normal, Add, Multiply, Screen, and Overlay and apply only to video/title
  clips.
- Audio gain is `[-96, +24]` dB, pan is `[-1, 1]`, fades are non-negative, individually fit the clip,
  and may not sum beyond its duration. Audio properties apply only to audio clips.
- Changes on a locked track fail atomically. The desktop coalesces adjacent updates to the same
  selected clip property into one undo step.

Audio-track mute and solo use `SetTrackAudioStateCommand`; gain/pan use
`SetTrackAudioMixCommand`. The target must be an existing audio track. Gain is `[-96, +24]` dB and
pan is `[-1, 1]`; values are revisioned, coalescible, undoable, persistent, and consumed after clip
mixing. Track effects use the typed `Effect` contract and are applied in canonical order: EQ,
compressor, dialogue noise reduction, limiter. Known parameters are range-validated before publish.
Track locking protects editorial structure but deliberately does not prevent mute/solo or mixer
gain/pan changes; it does protect effect-chain edits.

Effect keyframe times are clip-local and half-open at the clip duration. Values match the base
parameter type and are strictly time-ordered. Hold, Linear, and Bezier evaluation uses the
interpolation stored on the left keyframe. Bezier controls are normalized offsets from their owning
keyframe (incoming X left, outgoing X right), and segment time controls must remain monotonic.
Invalid curves fail before a revision is published. The CPU graph is the preview/export reference;
the current GPU path returns typed per-frame fallback for active effects.

## Titles and transitions

A title is authoritative clip state, not a viewer overlay. A `ClipKind::Title` clip has no media
asset and must carry exactly one `Title` payload. Media clips must not carry one. The title payload
contains UTF-8 text and font-family name, sequence-pixel font size, foreground and background RGBA,
horizontal alignment, bold, and italic. Validation bounds text at 64 KiB, the font-family name at
1024 bytes, and font size at 1–4096 pixels. Colors are finite normalized values. Changes use
`SetClipTitleCommand`, honor track locking, and follow normal revision, undo, and coalescing rules.

A transition is owned by the sequence and refers to an adjacent outgoing/incoming pair on one video
track. Its explicit half-open range must start inside the outgoing clip, end inside the incoming
clip, and strictly straddle their shared cut. The corresponding mapped source windows must stay
inside each participating media asset; generated titles do not require media handles. Enabled
transitions on the same track may not overlap. Supported kinds are `CrossDissolve` and
`DipToBlack`.

`AddTransitionCommand`, `UpdateTransitionCommand`, and `RemoveTransitionCommand` are canonical,
revision-checked edit operations. Any other command that would invalidate an existing transition is
rejected as one atomic edit, so a clip cannot silently leave a dangling or mistimed transition.
The CPU renderer is the correctness oracle. An active title or transition is an explicit unsupported
GPU timeline and invokes the per-frame CPU fallback rather than an approximate GPU result.

## Snapping

The edit model exposes a pure exact snapping query. When candidates are equally distant, priority
is playhead, marker, clip edge, frame grid, then deterministic entity identity. A request may
exclude selected/moving clip IDs and a dragged marker ID so an object cannot snap to either of its
own range boundaries. Marker creation uses no marker exclusion. The controller converts the widget
proposal and pixel threshold to exact `Time`, calls this query, and returns the winning point and
kind for drawing. Holding Shift bypasses the resolver. Keyboard nudging sends frame counts to the
controller, which uses exact `Rate::framesAt`/`frameTime` conversions and does not accumulate a
rounded NTSC duration.

## Interactive gesture contract

A pointer gesture may render transient geometry without mutating the project. On release it emits
the anchor/selection IDs, destination track, exact integer deltas in the widget timescale, mode,
intent, and snap state. The controller resolves current entities/revision and commits one command or
batch. Track-header, marker, and gap gestures use the same authoritative refresh boundary. Escape
cancels before commit. This prevents high-frequency mouse motion from generating journal entries or
an unmanageable undo history.

See [ADR 0005](../architecture/0005-precision-edit-commands.md) and
[ADR 0006](../architecture/0006-interactive-timeline-gestures.md). Title, transition, and schema-v2
decisions are recorded in [ADR 0013](../architecture/0013-schema-v2-titles-transitions.md).
Professional selection, batch, track, marker/gap, and snap boundaries are recorded in
[ADR 0014](../architecture/0014-professional-timeline-interaction.md).

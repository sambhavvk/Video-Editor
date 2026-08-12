# Edit model

`video_editor_edit_model` is the dependency-free C++20 editing core. Consumers
link the `VideoEditor::EditModel` target and include
`video_editor/edit_model/edit_model.h`.

Its time values are exact rational numbers. Any conversion which can lose
precision requires a `RoundingMode`; timeline comparisons and arithmetic use
128-bit intermediates and report overflow rather than silently wrapping.
The Windows build therefore requires clang-cl: GCC and Clang provide the
dependency-free `__int128` arithmetic used here, while native MSVC is rejected
at CMake configure time with an actionable error.

`TimelineEditor` is the sole mutation boundary. Commands carry an expected
revision, successful edits create immutable project states, and historical
`TimelineSnapshot` instances remain valid while later edits, undo, and redo
continue. Adjacent commands with the same non-empty coalescing key form one
undo step.

Current vertical-slice invariants:

- Sequence frame rate and canvas dimensions change together through one typed,
  revision-checked command; zero-sized canvases are rejected before mutation.
- Clips occupy half-open, non-overlapping ranges on a track.
- Media clips reference a project asset of the matching kind and must not carry
  title styling payloads. Title clips own a validated `Title` payload and do
  not reference media.
- Timeline and source starts are non-negative and durations are positive.
- Caption tracks reserve UI/model ordering but captions currently live on the
  sequence, which permits overlapping multilingual caption proposals.
- Move, trim, slip, and removal remain single-clip operations by default.
  Their explicit `include_linked` flags atomically include every member of the
  selected clip's linked group; this lets UI modifier keys opt in without
  changing legacy behavior. A linked move applies one exact timeline delta,
  while only the selected member may change tracks. Linked trim companions use
  their own speed/direction when deriving source boundaries.
- Roll edits require two contiguous neighbors and preserve their outer range.
  Slide edits require a contiguous previous/selected/next triplet and preserve
  both the selected source window and the triplet's outer range. All handle
  exhaustion fails atomically.
- Ripple insertion/removal is track-local unless linked removal is explicitly
  requested. Linked ripple removal requires matching companion ranges and
  closes the same exact interval on each affected track.
- Snapping returns all targets inside an inclusive exact threshold. Equal
  distances prefer playhead, marker, clip edge, and frame grid in that order,
  followed by stable time/identity/track/edge ordering.
- Unknown effects retain their version and opaque payload without evaluation.
- Sequence-owned transitions are exact timeline entities with explicit IDs,
  adjacent outgoing/incoming clip IDs, a half-open range straddling their cut,
  a typed kind, and an enabled flag. Enabled transitions are limited to adjacent clips on the
  same video track, must straddle their shared cut, require sufficient source
  handles for both sides, and cannot overlap another enabled transition on the
  same track.

## Clip property commands

Creative controls use the same revision and undo boundary as timeline edits.
The public operations are appended to `EditOperation` so every pre-existing
variant alternative keeps its ordinal:

```cpp
SetClipTransformCommand{sequence_id, clip_id, transform};
SetClipBlendModeCommand{sequence_id, clip_id, BlendMode::Overlay};
SetClipAudioPropertiesCommand{
    sequence_id, clip_id, -6.0, 0.0, Time(5, 10), Time(5, 10)};
SetTrackAudioStateCommand{sequence_id, audio_track_id, true, false};
```

Pass an operation through `TimelineEditor::apply` with the UI's expected
revision. For continuous inspector gestures, reuse one non-empty
`EditCommand::coalescing_key`; all updates still create immutable revisions,
but undo treats the gesture as one step.

Property validation is strict and atomic:

- Transform and blend commands accept video and title clips. Audio-property
  commands accept audio clips. The owning track must be unlocked.
- Position is measured in sequence-canvas pixels and is finite within
  `[-1,000,000, 1,000,000]` on each axis.
- Signed scale supports flipping. Each magnitude must be within
  `[0.0001, 1000]`; zero and non-finite values are rejected.
- Rotation is finite within `[-36,000, 36,000]` degrees. Anchors, crop values,
  and opacity are normalized values in `[0, 1]`. Opposing crop fractions must
  sum to less than one so some image area remains.
- Blend mode must be `Normal`, `Add`, `Multiply`, `Screen`, or `Overlay`.
- Audio gain is finite within `[-96, 24]` dB and pan within `[-1, 1]`. Fade
  durations are exact, non-negative timeline `Time` values whose sum cannot
  exceed the clip duration.
- A stale revision, missing sequence or clip, wrong clip kind, locked track,
  non-finite number, or out-of-range value leaves the project and undo history
  unchanged.

## Titles and transitions

Titles and transitions are canonical edit-model state rather than UI-only
presets:

```cpp
SetClipTitleCommand{sequence_id, clip_id, title};
AddTransitionCommand{sequence_id, transition};
UpdateTransitionCommand{sequence_id, transition};
RemoveTransitionCommand{sequence_id, transition_id};
```

- `SetClipTitleCommand` applies only to title clips on unlocked tracks.
  Title text and font family must be valid UTF-8. Text is limited to 64 KiB, the font-family name
  to 1024 bytes, font size to 1–4096 sequence pixels, and foreground/background colors to finite
  normalized RGBA.
- Transitions belong to the sequence instead of a specific track object so they
  survive clip-vector updates and serialize independently of track layout.
- Every transition command is revision checked, undoable, and coalescible like
  other inspector edits.
- Any other edit which would leave an existing transition invalid is rejected
  atomically during whole-project validation.

Use `findTransition(sequence, id)` to inspect the authoritative transition
record in an immutable snapshot.

Audio-track mute and solo are also revisioned project state. Only audio tracks
accept `SetTrackAudioStateCommand`; missing sequences or tracks and video or
caption tracks fail atomically. A track lock does not block this command:
locking protects editorial changes to track contents, while mute and solo are
mixer controls. Adjacent mixer updates may use one coalescing key so a drag or
paired mute/solo gesture remains one undo step.

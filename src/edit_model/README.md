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
- Media clips reference a project asset of the matching kind; title clips have
  no asset requirement.
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

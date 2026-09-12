# E02 — Trim previews with frame deltas and two-up comparison

Implemented on branch `beta-1.0-fix` (phase 3 E02).

## Timeline previews

- Primary gesture ghosts show an exact frame delta badge (`+Nf` / `-Nf`) during trim, roll, slide,
  move, and slip drags.
- Affected neighbors render as lighter dashed ghosts for ripple trims, roll partners, and slide
  neighbors so editors can predict which clips will move before commit.
- Escape cancels without committing; the precision trim status and program two-up compare clear on
  cancel or commit.

## Program two-up compare

- Trim in/out, roll, and slip gestures fetch outgoing/incoming source frames into the program viewer
  split compare while dragging.
- Slip compares source windows before and after the proposed source shift.

## Tests

- `EditorWindowTest::timelineEmitsTrimPreviewStatusDuringGestures`
- `EditorWindowTest::programViewerRestoresProgramFrameAfterTrimCompare`
- Existing trim, roll, slip, slide, and cancel tests remain green.

## Residual limitations

- Overwrite-trim neighbor collisions are not fully ghosted on the timeline.
- Move and ripple-trim previews do not yet shift downstream clips on other tracks (linked A/V).
- Slide previews are not clamped to neighbor source handles; invalid durations are omitted.
- Slip two-up still uses Out/In labels for the before/after in-point compare.

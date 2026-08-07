# ADR 0005: Precision edits and linked-clip command semantics

- Status: Accepted
- Date: 2026-08-06
- Owners: Core/Media and Desktop/Product

## Context

Roll, slip, slide, snapping, and linked A/V edits must remain exact, undoable, and usable by any UI
surface. Encoding those rules only in pointer handlers would make keyboard, command-palette, and
future interchange behavior diverge.

## Decision

- Roll, slip, and slide are typed edit-model commands applied with an expected revision.
- Linked behavior is opt-in on move, trim, remove, and slip commands. Defaults retain the original
  single-clip behavior so existing callers do not silently mutate companions.
- A linked move applies one exact timeline delta; only the selected clip may change tracks. Linked
  trims derive each companion's source change from its own rate and direction. A linked ripple
  removal closes the corresponding gap independently on every affected track.
- Snapping is a pure edit-model query over exact `Time` values. Stable equal-distance priority is
  playhead, marker, clip edge, then frame grid, followed by deterministic identity ordering.
- Pointer gestures may preview freely, but commit one typed command on release. Adjacent updates
  with one gesture key coalesce into one undo entry.

## Consequences

All editing surfaces share the same validated invariants and deterministic snap result. The model
rejects out-of-bounds source windows, overlaps, locked linked companions, and non-adjacent roll
pairs without partially changing a revision.

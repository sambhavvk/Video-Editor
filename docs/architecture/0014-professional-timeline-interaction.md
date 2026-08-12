<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0014: Professional timeline interaction boundary

- **Status:** accepted
- **Date:** 2026-08-11
- **Owners:** Core/Media and Desktop/Product

## Context

The beta timeline must support multi-selection, linked A/V operations, professional trim tools,
track management, markers, gaps, and exact snapping without creating a second edit model inside
the Qt widget. Pointer previews must remain responsive, while a completed gesture must still be one
revision-checked, undoable project edit.

## Decision

### Selection and atomic edits

- Clip, marker, and gap selection is transient presentation/controller state. It is not serialized
  into the project and does not create revisions.
- A clip click replaces selection, Control toggles membership, and Shift extends from the selection
  anchor in deterministic visual order. Marker and gap selection are exclusive with clip selection.
- A batch edit is applied to one candidate project and publishes one revision and one history entry.
  Any failed member rejects the complete batch. The controller no longer emulates atomicity by
  applying commands one at a time and undoing failures.
- Linked split commands carry the right-half identities for every participating clip. Generated
  identities never appear only as an implementation side effect, so command results are stable.
  Linked split and delete include every clip in the selected linked group and fail atomically when
  any participant is locked or invalid.

### Trim and tool semantics

- Normal, ripple, and overwrite trim are model policies, not pointer-handler algorithms. Ripple
  trim moves following material by the exact duration change; overwrite trim preserves sequence
  positions and removes or edge-trims material covered by an extension. A shrink may leave a gap.
- Roll, slip, and slide pointer tools emit transient previews and one commit. The controller maps
  the committed intent to the existing exact typed commands and the model remains responsible for
  adjacency, source handles, locks, rates, reverse mapping, and overlap validation.
- Keyboard nudging is expressed in frame counts. The controller converts frame numbers through the
  sequence's exact `Rate`; the widget does not accumulate a rounded frame duration.

### Tracks, markers, gaps, and snapping

- Track name, order, lock, output visibility, and targeting are canonical sequence state changed by
  typed commands. Visibility controls video/title contribution to preview and export. Targeting
  selects the compatible destination used by insert/overwrite operations.
- Gaps remain derived half-open ranges and do not receive persistent entity IDs. The presentation
  layer gives a visible gap a revision-local key made from track identity and exact range. Closing
  a selected gap is a typed edit that shifts later material; stale keys are resolved against the
  current snapshot and rejected instead of editing a different gap.
- Marker creation, selection, movement, rename, and removal use the canonical marker commands.
- The timeline receives a synchronous presentation-level snap resolver from the controller. The
  resolver converts the pointer proposal and pixel threshold to exact `Time`, calls the edit-model
  snapping query, and returns the winning point and kind. Shift disables the request. Moving clips
  and a marker being dragged are excluded from their own boundary candidates. Marker creation has
  no exclusion. Marker, playhead, clip-edge, and frame-grid ties use the edit model's stable
  priority.

### Qt interaction and accessibility

- Track headers and timeline objects expose visible text, tooltips, keyboard equivalents, focus
  indication, and accessible names/descriptions. State is communicated by text/icon shape as well
  as color.
- Escape cancels any active clip, marker, or track-order gesture without a revision. Invalid drops
  restore the authoritative snapshot and show a plain-language error. Direct manipulation previews
  remain UI-only and respond without waiting for media rendering.

## Consequences

The Qt surface stays independent of edit-model types while using the exact model for all committed
geometry and snap decisions. Multi-clip and linked edits become truly atomic. Track presentation
state round-trips through schema v2, and older v2 payloads default new track flags safely. Gap keys
are deliberately not stable across revisions, preventing derived empty space from becoming
authoritative project state.

## Verification

- Model tests cover batch rejection, one-step undo/redo, linked split/delete, ripple and overwrite
  trim, gap close, track commands, locks, exact NTSC nudging, and snap exclusion/tie behavior.
- Codec tests cover track-state round trips and old-v2 defaults.
- Qt tests cover modifier selection, every tool mode, one-preview/one-commit/cancel, marker/gap and
  track-header interaction, focus/keyboard behavior, and canonical resolver use.
- Controller tests cover exact command mapping, batch edits, targeted insertion, marker/gap edits,
  persistence refresh, and deterministic undo/redo.

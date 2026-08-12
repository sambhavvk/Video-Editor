<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0006: Interactive timeline gesture boundary

- **Status:** accepted
- **Date:** 2026-08-08
- **Owners:** Desktop/Product and Core/Media

## Context

The virtualized Qt timeline needs responsive move and edge-trim feedback without turning every
pointer event into a project revision. It also needs keyboard nudging, cancel behavior, snapping,
autoscroll, and ripple/overwrite intent to converge on the same edit-model commands.

## Decision

- Pointer movement changes only transient presentation state. On release the timeline emits one
  commit containing the anchor clip, selected clip IDs, destination track index, start/duration
  deltas in its integer timescale, edit mode, edit intent, and whether snapping was applied.
- The application controller resolves the ID against the current sequence and converts that commit
  into a typed revision-checked move or trim command. The edit model remains responsible for track
  compatibility, overlap, source handles, linked companions, locking, and atomic rejection.
- Escape cancels an active gesture with no edit revision. Moving beyond the viewport edge may
  autoscroll without changing commit semantics.
- Shift temporarily disables snapping. Control requests ripple intent and Alt requests overwrite
  intent. Keyboard nudging emits a frame count; the controller resolves it through the sequence's
  exact rate instead of adding a rounded widget tick.
- The surface asks a controller-provided resolver for snap results. The resolver calls the pure
  edit-model query, including marker and exact frame-grid candidates and excluding the moving
  selection. The widget draws the returned kind and position and owns no competing snap priority.
- Move, trim, split, delete, and precision tools enable their defined linked A/V behavior at the
  controller boundary. A multi-selection commit is one atomic batch revision.
- Track-header, marker, and gap gestures follow the same preview/commit/cancel boundary. Selection
  remains transient, while track/marker changes and closing a derived gap are typed commands.

## Consequences

- A drag produces one undoable, journaled command rather than hundreds of revisions.
- The UI can preview invalid geometry, but release either commits a wholly valid model operation or
  returns to the authoritative snapshot with a plain-language error.
- Pointer, keyboard, and future command-palette tools share exact delta and intent contracts.
- Roll, slip, slide, marker/gap interaction, track management, and ripple/overwrite trim use the
  same authoritative refresh-after-commit behavior as move and normal trim.

## Verification

Qt interaction tests cover modifier/range selection, tool and header hit regions, preview and
single commit, resolver use and Shift bypass, Escape cancel, autoscroll, marker/gap interaction,
accessibility, and frame-count nudging. Edit-model and controller tests independently cover exact
committed-operation invariants and batch atomicity.

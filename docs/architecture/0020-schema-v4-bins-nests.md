<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0020: Schema v4 bins, metadata, and nested-sequence fields

- **Status:** accepted
- **Date:** 2026-09-05
- **Owners:** Core/Media and Quality/Platform

## Context

Beta editors need folder bins, smart collections, and project-owned asset metadata so media
organization survives save and recovery without depending on the media cache. Nested sequences must
also be representable in the edit model and snapshot format before recursive flattening, sequence
tabs, or GPU/audio nest expansion land in later work.

## Decision

### Canonical edit model

- `MediaBin` entities are first-class project state. Folder bins may parent other folder bins and
  receive filed assets. Smart bins store a `SmartQuery` and never own assets directly.
- `Asset` gains optional `bin_id`, `display_title`, `tags`, `notes`, and `rating` (0–5, 0 =
  unrated). Project asset metadata is authoritative; the media cache may mirror it but must not
  replace it on load when project fields are already set.
- `ClipKind::NestedSequence` clips carry `nested_sequence_id`, use a nil `asset_id`, forbid title
  payloads, and are accepted on video tracks. Validation rejects parent cycles and nesting depth
  greater than eight. Renderers treat nested clips as schema-only placeholders until flattening is
  implemented.
- Typed commands (`CreateBin`, `RenameBin`, `MoveBin`, `RemoveBin`, `SetAssetBin`,
  `SetAssetMetadata`, `SetSmartQuery`) are revision checked, atomic, and undoable.

### Project snapshot schema v4

- Writers emit schema version 4 with minimum reader version 1. Protobuf field numbers are appended;
  existing numbers are never reused.
- Readers accept declared schema versions 1–4. Older declared versions cannot smuggle v4 bins,
  asset metadata, or nested-sequence fields.
- Saving a project opened from an older snapshot writes canonical v4 bytes.

### SQLite project-store schema v2 and recovery

- Journal snapshots are recorded as `project.snapshot.v4` with `payload_schema_version` 4. Readers
  continue to accept `project.snapshot.v1`, `v2`, and `v3`.
- The SQLite store schema remains v2; only the protobuf payload version advances.

### Desktop UI

- The media bin shows a folder/smart tree plus the existing results table. Search matches name,
  tags, notes, and rating. Inspector metadata edits apply `SetAssetMetadata` commands.

## Consequences

Media organization and nested-sequence structure round-trip through schema v4 while older projects
remain readable. CPU preview renders nested clips as black placeholders; GPU reports unsupported
clip kind without crashing. Recursive flatten, nest UI, and nested playback remain follow-up work.

## Required verification

- Bin command validation, metadata validation, nested clip cycle/depth rejection, undo, and redo.
- Canonical v4 bytes, v1–v3 upgrade compatibility, declared-version field enforcement, and
  unsupported-future rejection.
- Desktop offscreen media-bin tree/search coverage and accessibility names for new controls.

## Addendum (2026-09-05): nested playback and editing

- CPU and GPU timeline renderers recursively evaluate child sequences via `source_time_for` /
  `source_range` mapping (outside the mapped child window → black video). Depth is capped at eight.
- Timeline audio mixes `NestedSequence` clips on video tracks into the parent master output; muted
  video tracks silence their nested audio. Child-track mute/solo apply inside recursion.
- The desktop shell exposes sequence tabs, **Nest selected clips** (Edit/Timeline menus), and opens
  the child sequence when a nested clip is activated. `EditorController::currentSequence()` follows
  the active tab for preview and export.

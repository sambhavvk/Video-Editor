<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0013: Schema v2 title and transition contracts

- **Status:** accepted
- **Date:** 2026-08-11
- **Owners:** Core/Media and Quality/Platform

## Context

The beta timeline needs titles and transitions to be authoritative project state rather than UI
presets. Those entities must survive save, recovery, and migration, and preview/export must share a
deterministic reference result. Existing schema-v1 snapshots contain `ClipKind::Title` but no title
payload, contain no transitions, and the GPU compositor deliberately rejects title clips.

## Decision

### Canonical edit model

- A title clip owns an optional `Title` payload. The payload is required when the clip kind is
  `Title` and forbidden for media clips. It stores UTF-8 text, font-family metadata, size in
  sequence pixels, premultiplied-compatible foreground and background colours, horizontal
  alignment, bold, and italic state. Clip transform and blend properties remain the
  non-destructive placement/compositing controls.
- A `Transition` is a sequence entity with its own ID, outgoing and incoming clip IDs, an explicit
  half-open timeline range, a transition kind, and an enabled flag. Cross Dissolve and Dip to
  Black are the schema-v2 kinds.
- A transition pair must be two distinct adjacent clips on the same video track. Its range must
  contain time on both sides of their shared cut. Source handles required by the pre-cut incoming
  and post-cut outgoing portions must remain inside each media asset; generated titles do not need
  media handles. Transition ranges may not overlap another enabled transition on the same track.
- `SetClipTitleCommand` and transition add/update/remove commands are typed, revision checked,
  atomic, coalescible, and undoable. Any edit that would leave an existing transition invalid is
  rejected rather than silently changing or deleting authoritative state.

### Project snapshot schema v2

- Writers emit schema version 2 with minimum reader version 1. Protobuf field numbers are appended;
  existing field numbers and enum values are never reused.
- Readers accept declared schema versions 1 and 2. A v1 title clip is upgraded in memory with a
  deterministic default title whose text is the clip name. V1 payloads are not allowed to smuggle
  fields introduced by v2 under an older declared version.
- Newer schema versions, newer minimum-reader requirements, malformed fields, duplicate IDs, and
  unknown fields continue to fail atomically. Reading never returns a partially decoded project.
- Saving a project opened from v1 writes canonical v2 bytes. Older beta readers are not expected to
  read projects after they have been saved as v2.

### SQLite project-store schema v2 and recovery

- The v1-to-v2 migration adds a positive `payload_schema_version` to journal entries. Existing
  entries become version 1; new schema-v2 snapshot entries are explicitly version 2.
- Migration remains adjacent, forward only, and transactional. Before modifying an established v1
  database, the online-backup path writes, checks, fsyncs, and atomically publishes a
  `.pre-migration-v1.bak` sibling.
- The read-only recovery catalog recognizes supported v1 and v2 candidates without mutating them.
  Opening a selected v1 candidate performs the normal backed-up migration while retaining its
  prior clean-close and unsaved-revision diagnosis.

### Reference rendering and GPU fallback

- `CpuRenderer` is the reference implementation. It rasterizes title text without asking the media
  frame provider and evaluates Cross Dissolve and Dip to Black with exact range decisions and a
  deterministic floating-point blend factor.
- Transition source time is the ordinary clip mapping extrapolated into validated media handles.
  Cross Dissolve blends complete outgoing and incoming track results over the same lower-track
  baseline. Dip to Black reaches opaque black at the shared cut.
- The built-in title rasterizer is deterministic and dependency-free. Unsupported glyphs use a
  visible replacement glyph; the stored UTF-8 and font metadata are preserved for later shaped-text
  renderers.
- The GPU timeline renderer reports `GpuUnsupportedTimeline` for an active title or transition.
  The desktop's existing per-frame CPU fallback handles that result without latching the GPU as
  failed. Preview and reference export therefore stay correct while native GPU title/transition
  shaders remain follow-up optimization work.

## Consequences

Schema-v1 projects remain readable and recoverable, while every new title and transition is
revisioned and round-trips through schema v2. CPU preview/export establish one golden result. GPU
acceleration remains available for supported frames and truthfully falls back only where the v2
graph requires the reference path.

## Required verification

- Title and transition command validation, stale revisions, atomic failure, coalescing, undo, and
  redo.
- Canonical v2 bytes, v1 upgrade fixtures, v2 round trips, declared-version field enforcement, and
  unsupported-future rejection.
- Successful and failed v1-to-v2 migration, pre-migration backup integrity, read-only v1 recovery
  discovery, and preservation of recovery status after opening.
- Golden CPU title, dissolve, and dip frames; handle-boundary failures; provider-call behavior; and
  typed, non-device-failure GPU fallback.

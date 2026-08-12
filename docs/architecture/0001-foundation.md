<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0001: Native modular editor foundation

- **Status:** accepted
- **Date:** 2026-08-06

## Decision

Use C++20 and a dependency-free edit model. Build the desktop shell with Qt Widgets, media I/O with
a pinned LGPL-compatible FFmpeg build, image processing with libplacebo, and project persistence
with SQLite. Background work communicates through a versioned Protobuf protocol.

The edit model owns timeline semantics. FFmpeg, libplacebo, Qt, SQLite, and worker-specific types
must not appear in its public API. Preview and export compile the same immutable timeline snapshot
into the same render graph; quality policy is the only allowed difference.

## Consequences

- Engine correctness can be tested without Qt, a GPU, or media libraries.
- UI, storage, media, rendering, audio, and workers can be developed in parallel behind contracts.
- Dependency builds and their licenses are release inputs, not incidental system configuration.
- Public schema and protocol changes require migrations and compatibility fixtures.

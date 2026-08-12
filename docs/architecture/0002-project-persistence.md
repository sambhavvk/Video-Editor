<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0002: SQLite working database and checkpoint project files

- **Status:** accepted
- **Date:** 2026-08-06

## Decision

Each open project uses a local SQLite working database in WAL mode with `synchronous=FULL`. A
completed edit gesture appends one versioned command and updates materialized state in one
transaction. The portable `.veproj` file is a checkpoint made through SQLite's backup API, fsync,
and atomic replacement; it never depends on a sibling WAL file.

Media, proxies, thumbnails, waveforms, render caches, and transcription models are not authoritative
project data and remain outside the checkpoint.

## Consequences

- A crash can recover every committed edit while an active gesture may be discarded.
- Project files can be copied independently of runtime WAL/SHM files.
- Forward migrations are transactional and always preserve a pre-migration backup.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# Project format and recovery

The native `.veproj` is a self-contained SQLite checkpoint. It stores editorial state and its
journal, not media or caches. While a project is open, the application edits a separate local
working database under the platform's application-data recovery directory.

## Stored state

The current store schema version is 2. The working/checkpoint database contains
three tables:

- `project_metadata`: the project UUID, schema version, head revision, saved revision, clean-close
  flag, and heartbeat timestamp;
- `command_journal`: one monotonically increasing revision per committed payload, including the
  deterministic `project.snapshot.v1`/`project.snapshot.v2` records retained from older projects
  and `project.snapshot.v3` records written by the current application, plus a positive
  `payload_schema_version` column recording the schema carried by each entry;
- `schema_migrations`: applied forward migration records.

The snapshot Protobuf has independent `schema_version` and `minimum_reader_version` fields and
serializes assets, sequences, tracks, clips, source/timeline ranges, transform and audio fields,
typed effects/keyframes, markers, captions with timed words/provenance/canonical style, canonical
title payloads, and sequence-owned transitions. The current snapshot schema is v3; v1 and v2
payloads remain readable with their historical caption defaults, while older declared schemas may
not smuggle newer fields. Track name/order, lock, output visibility, targeting, mute/solo, gain/pan,
and typed audio effects are serialized with the sequence; transient clip/marker/gap selection and
derived gap keys are not. Unknown future effects remain opaque and disabled so a compatible reader can
round-trip their payload without applying unknown processing.

Paths, fingerprints, and media descriptors are project references. Original media bytes, proxy
files, `.vepts` maps, thumbnails, waveforms, render cache, and model downloads are not stored.

## Working database

An open working database uses:

```text
PRAGMA foreign_keys = ON
PRAGMA journal_mode = WAL
PRAGMA synchronous = FULL
```

Opening marks it unclean. A completed edit first succeeds in the exact edit model, then the
controller appends the serialized new snapshot at the store's expected head revision and updates
the heartbeat. If the store write fails, the controller attempts to undo the just-applied model
change and reports a project-write error.

WAL and SHM files are runtime details of the local working database. They are not portable project
components and must not be copied as a save operation.

## Checkpoint save

Saving to `.veproj` follows this sequence:

1. Reject the open working path as a destination.
2. Create a unique temporary sibling database.
3. Copy a consistent snapshot with SQLite's online backup API.
4. Set the destination copy to rollback-journal mode and a clean saved revision.
5. Run integrity and schema validation and reject live SQLite sidecars.
6. Flush the database file; on POSIX, fsync it and its directory.
7. Atomically replace the destination.
8. Only after successful replacement, advance `saved_revision` in the working database.

An interruption before atomic replacement leaves the prior checkpoint intact. A `.veproj` does not
need a sibling WAL file to open. Save As appends `.veproj` when no matching suffix is present.

## Opening a checkpoint

The desktop copies the checkpoint into a newly named local working database, opens and validates
it, then reads the latest supported `project.snapshot.v1`, `project.snapshot.v2`, or
`project.snapshot.v3` entry. The
journal type must agree with `payload_schema_version`, and the embedded snapshot declaration is
validated independently. Declared schema-v1 snapshots are upgraded by the codec's backward reader;
declared schema-v2 snapshots round-trip title, transition, track-interaction, and track-audio state
directly. Older schema-v2 payloads written before additive track fields existed decode
visibility/targeting as enabled and gain/pan as neutral through presence-aware defaults. Schema-v3
adds timed caption words, provenance, and renderer-actionable style while retaining all v2
editorial fields. The decoded project ID must match the store metadata for recovery; invalid schemas
or snapshots are rejected with an error
rather than partly opening a project.

Imported runtime `AssetRecord` data and proxy manifests are currently not rebuilt completely from
the project snapshot. Original paths are registered for playback, but proxy association and some
media-bin details are session-oriented. Persistent media reconstruction is therefore still a beta
gap.

## Startup recovery catalog

The recovery scan is deliberately read-only:

- it examines only direct `*.working.sqlite` children of the recovery directory;
- it does not recurse, create a missing directory, migrate candidates, change clean-close state, or
  update heartbeats;
- it opens candidates read-only, performs bounded `quick_check`, validates schema/tables and journal
  head, and records a bounded diagnostic for invalid candidates;
- it recommends recovery when `clean_close` is false **or** `head_revision != saved_revision`;
- valid recommended candidates sort before clean candidates, then by newest heartbeat, with a
  deterministic path tie-breaker;
- a configurable maximum bounds the inspected catalog and reports truncation.

At startup the current desktop offers the first valid recommended candidate other than its active
working path. Accepting opens the latest committed project snapshot and marks the project dirty.
Declining does not delete or modify the candidate.

`clean_close` means shutdown finalization completed, not that all changes were saved. Conversely, a
clean project may still have a head newer than its saved checkpoint; these are independent recovery
signals.

## Migrations and compatibility

Migrations are forward-only and transactional. Before migrating a supported older database, the
store creates and validates a `.pre-migration-vN.bak` copy through the same online-backup and atomic
commit principles. A database newer than the current reader, or an unrecognized version-zero
database containing an unrelated schema, fails closed.

The supported SQLite store migration path today is store schema v1 → v2. That migration adds
`command_journal.payload_schema_version`, preserves recovery metadata, and creates a validated
`.pre-migration-v1.bak` before mutating the database. The migration mechanism is tested, but
compatibility across a longer beta schema history cannot be claimed until those schemas exist and
have fixtures.

## Failure expectations

The implementation is designed so a crash may discard an in-progress pointer gesture but not a
completed journal append. It does not make filesystem hardware infallible. Disk failure, corrupt
media, and broader process-termination fault injection remain release gates. Keep external backups
of important `.veproj` checkpoints during the engineering-preview phase.

See [ADR 0002](../architecture/0002-project-persistence.md) and
[ADR 0008](../architecture/0008-recovery-catalog.md), plus
[ADR 0013](../architecture/0013-schema-v2-titles-transitions.md) and
[ADR 0014](../architecture/0014-professional-timeline-interaction.md), and
[ADR 0018](../architecture/0018-local-transcription-and-caption-proposals.md).

<!-- SPDX-License-Identifier: MPL-2.0 -->

# `ProjectStore` API reference

Header: `video_editor/project_store/project_store.hpp`

Namespace: `video_editor::store`

`ProjectStore` owns one local SQLite connection and the working database identified by
`working_path()`. It is move-only. The application must externally serialize calls; one instance is
not a cross-thread synchronization primitive.

Construction opens or creates the database, performs requested integrity checks, migrates supported
older schemas, captures the previous recovery state, and marks the active working database unclean.
The destructor deliberately does not claim a clean close; call `mark_clean_close` only after all
shutdown work succeeds.

## Schema and journal

The current store schema is 2 and the minimum supported schema is 1. Schema v2 adds the positive
`JournalEntry::payload_schema_version`. Existing v1 entries migrate with value 1. Migration first
publishes and validates a sibling `.pre-migration-v1.bak`, then applies the schema step and metadata
update in one transaction.

`append_command` accepts text or binary payloads, an expected head revision, and an optional payload
schema version. It commits one next revision or throws `RevisionConflict`. `read_commands` returns
owned entries after the requested revision.

## Save and recovery

`checkpoint_to` uses SQLite online backup into a temporary sibling, validates and flushes the copy,
atomically replaces the destination, and only then advances `saved_revision`. It rejects the active
working path.

`recovery_status()` borrows the status captured before this process marked the database open.
`scan_recovery_directory` is a read-only free function: it neither migrates candidates nor changes
their heartbeat or clean-close state. Both schema-v1 and schema-v2 candidates are discoverable.

`metadata`, `quick_check`, and `read_commands` return owned values. `working_path` and
`recovery_status` return references whose lifetime is bounded by the owning store.

AI assistance has been used to create this output.

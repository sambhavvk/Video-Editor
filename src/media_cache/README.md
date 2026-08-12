<!-- SPDX-License-Identifier: MPL-2.0 -->

# Media cache

The `media_cache` module owns rebuildable media artifacts — thumbnails,
waveforms, and decoded metadata — that live outside the `.veproj` project
file. The artifacts are derived data: regenerating them is cheap relative to
the source media, so the cache may be deleted at any time without losing edits
or original media.

`CacheStore` is a content-addressed on-disk blob store. Each entry is keyed by
`(asset_id, kind, parameter_hash)` and stored as one blob file under
`<root>/blobs/<sha256-of-key>`, with a SQLite index at `<root>/index.sqlite`
recording size and access time. The store enforces a configurable byte budget
and evicts least-recently-used entries to stay within it. `put` writes the blob
to a temp file, fsyncs it on POSIX, and renames it into place so a crash never
leaves a partially written blob referenced by the index.

The store is not thread-safe. Callers serialize access through the owning
service. Deleting the cache directory never destroys project edits or
originals — only the derived artifacts are lost, and they are rebuilt on
demand.

## Platform notes

On POSIX systems the blob file and its parent directory are fsynced before the
index is updated, so a crash after `put` returns leaves a durable, consistent
entry. On Windows the blob is written and renamed but not fsynced; the index
uses `PRAGMA synchronous = FULL` for its own durability. Full Windows blob
durability will be added in a follow-up.

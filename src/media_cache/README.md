<!-- SPDX-License-Identifier: MPL-2.0 -->

# Media cache

The `media_cache` module owns rebuildable media artifacts — thumbnails,
waveforms, and decoded metadata — that live outside the `.veproj` project
file. The artifacts are derived data: regenerating them is cheap relative to
the source media, so the cache may be deleted at any time without losing edits
or original media. Completed editing proxies share this store so one LRU budget
covers thumbs, waveforms, metadata, and proxies.

`CacheStore` is a content-addressed on-disk blob store. Each entry is keyed by
`(asset_id, kind, parameter_hash)` and stored as one blob file under
`<root>/blobs/<sha256-of-key>`, with a SQLite index at `<root>/index.sqlite`
recording size and access time. Kinds include Thumbnail, Waveform, Metadata,
Proxy, and ProxyPtsMap. `put` writes small blobs from memory; `put_file`
atomically adopts an existing file without reading it into RAM; `path_for`
returns the on-disk blob path for FFmpeg/playback. The store enforces a
configurable byte budget and evicts least-recently-used entries to stay within
it. Writes use a temp file, fsync on POSIX, and rename so a crash never leaves
a partially written blob referenced by the index.

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

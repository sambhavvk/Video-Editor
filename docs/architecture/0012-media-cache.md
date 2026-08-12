<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0012: Rebuildable media cache (thumbnails, waveforms, metadata)

- **Status:** Accepted
- **Date:** 2026-08-11
- **Owners:** engine / desktop

## Context

The public-beta feature matrix lists "Metadata, thumbnails, waveforms" as
**Missing**: no persistent metadata editor, thumbnail service, waveform
pyramid, or visible media thumbnails. The architecture overview already
classifies these as **rebuildable artifacts** that must stay outside `.veproj`
(see ADR 0001 and the overview's "Authoritative and rebuildable state" section),
but no implementation existed.

Three distinct capabilities are needed:

1. **Thumbnails** — small JPEG stills for the media-bin grid and clip headers,
   extracted from a chosen source frame.
2. **Waveforms** — multi-resolution audio amplitude pyramids for clip headers
   and the timeline, drawable at any zoom without re-decoding.
3. **Metadata** — a per-asset user-editable document (title, tags, notes,
   rating, custom fields) distinct from the probed technical metadata that
   lives in the edit model.

All three share the same shape: a content-addressed blob keyed by asset and
generation parameters, produced by an extractor, with a pure/in-memory core and
a thin I/O boundary. The beta matrix also calls out a missing "dependency-hash
cache inventory, 100 GB configurable budget, disk LRU, inspection, or eviction
UI" — a single shared cache store addresses all of those for this class of
artifact.

## Decision

Add one new module, `media_cache`, containing a shared content-addressed store
and three services that build on it.

### `CacheStore`

- On-disk blob store: one file per entry under `<root>/blobs/<sha256-of-key>`,
  SQLite index at `<root>/index.sqlite`.
- Keyed by `(asset_id, CacheKind, parameter_hash)`. The parameter hash lets a
  service supersede an entry when its generation parameters change without
  invalidating the whole asset.
- Configurable byte budget (default 100 GB, matching the documented beta
  target) with least-recently-used eviction. `put` protects the just-inserted
  entry from eviction so a large new entry cannot evict itself.
- Atomic writes: temp file + fsync (POSIX) + rename, then index update. A crash
  never leaves a partially written blob referenced by the index.
- `inspect()` returns an LRU-ordered inventory for a future cache browser.
- Not thread-safe; callers serialize through the owning service.
- Rebuildable: deleting the directory never destroys edits or originals.

### `thumbnail_service`

- Pure resolvers (`thumbnail_target_dimensions`, `thumbnail_source_pts`,
  `thumbnail_parameter_hash`) are exposed for unit testing without FFmpeg.
- `generate_thumbnail` opens the source via FFmpeg, seeks to a deterministic
  frame (First/Middle/Last strategy, Middle = floor(duration/2)), decodes one
  frame, scales to the long edge preserving aspect ratio (no upscale, even
  dimensions for YUV420P), and encodes MJPEG.
- Cancellable via `std::stop_token` with an FFmpeg interrupt callback.
- Results are stored in and loaded from `CacheStore`; a cached entry is
  returned without re-decoding.

### `waveform_service`

- Pure resolvers and pyramid builders (`waveform_level_bucket_counts`,
  `build_waveform_level`, `build_waveform_pyramid`) are exposed for unit
  testing without FFmpeg.
- `generate_waveform` decodes the best audio stream, resamples to mono float32
  at 48 kHz via swresample, and downsamples into a pyramid of `{min, max, rms}`
  buckets. Each level halves the bucket count.
- Empty buckets use a sentinel (`min=1, max=-1`, i.e. min > max) so the UI can
  distinguish "no data" from "silence".
- Compact little-endian binary serialization with magic `VEWAVE01`; readers
  reject unknown magic, negative counts, non-sentinel min>max buckets, and
  trailing bytes.

### `metadata_service`

- `MetadataDocument` (title, tags, notes, rating 0–5, ordered custom fields) is
  the backing store for the future metadata editor panel.
- Distinct from `media::AssetDescriptor` technical metadata, which is
  authoritative and lives in the edit model. This service owns only the
  user-facing editorial layer.
- Compact little-endian binary serialization with magic `VEMETA01`; readers
  reject unknown magic/version, out-of-range rating, empty or duplicate custom
  keys, and trailing bytes.
- One document per asset, keyed by a fixed parameter hash `"v1"`.

### Module boundaries

`media_cache` depends on `media_codec` (for `AssetDescriptor`/`VideoDescription`
types) and the pinned FFmpeg ABI (thumbnail decode/encode, waveform
decode/resample). It does not depend on the edit model, project store, or Qt.
The application controller will compose it with `asset_service` (which already
produces the `AssetRecord` and fingerprint used as the cache key).

## Consequences

- The "Metadata, thumbnails, waveforms" beta row moves from **Missing** to
  **Partial**: the cache store, extractors, serializers, and unit tests are
  implemented; desktop UI wiring (media-bin thumbnails, clip-header waveforms,
  metadata editor panel) and FFmpeg-dependent integration tests are the
  remaining work.
- The cache store is the foundation for the broader "Cache management" row:
  the 100 GB budget, LRU eviction, and `inspect()` inventory now exist. A cache
  browser UI and cross-module budget sharing with proxies remain future work.
- The store is deliberately not thread-safe; the application controller must
  serialize access. This matches the existing in-process proxy/export pattern.
- FFmpeg-dependent generation paths are not exercised by unit tests (which
  cover only the pure resolvers and serializers). Integration tests against
  generated fixtures, like `tests/proxy_service`, are a follow-up.
- Windows blob fsync is deferred; the index uses `PRAGMA synchronous = FULL`
  for its own durability in the meantime.

## Open items

- Wire `media_cache` into `editor_controller`: generate thumbnails on import,
  render thumbnails in `MediaBinWidget`, draw waveforms in `timeline_widget`,
  add a metadata editor panel.
- Add FFmpeg-dependent integration tests with generated media fixtures.
- Share the cache budget across proxies, thumbnails, and waveforms (currently
  each artifact class has its own store root).

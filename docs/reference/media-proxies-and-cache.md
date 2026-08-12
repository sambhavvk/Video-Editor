<!-- SPDX-License-Identifier: MPL-2.0 -->

# Media, proxies, and cache behavior

The editor references media in place. Original files are authoritative for export; a proxy is a
rebuildable playback optimization and never changes the edit model's source timing.

## Import and fingerprints

`asset_service` probes media through the pinned FFmpeg ABI and records a file URI, quick SHA-256
fingerprint, size and modification time, streams, duration, codecs, dimensions, nominal rate, and
audio layout. A full SHA-256 can be requested by the lower-level API but is not the default desktop
import path.

The project asset stores the source URI, quick fingerprint, high-level media attributes, and select
metadata. Import accepts the best video and audio stream for current editing. Broader stream
selection, image sequences, attachments, alternate angles/languages, and persistent relink UX are
not complete.

The lower-level relink contract compares fingerprints and refuses changed content unless the caller
explicitly permits it. The desktop's Relink menu item is currently not connected.

## Proxy recommendation and profile

The asset policy recommends a proxy for difficult/high-resolution video. The desktop does not
automatically enqueue it: the user right-clicks the media-bin item and chooses **Create editing
proxy**.

The requested default is:

- half resolution, bounded to 1920×1080;
- ProRes Proxy video in QuickTime/MOV;
- signed 16-bit PCM audio at 48 kHz when audio is requested;
- FFV1 in Matroska as the predetermined patent-neutral fallback when the requested encoder profile
  cannot be resolved and fallback is allowed.

The proxy service scans the complete input stream table but transcodes only the best decodable
video and, optionally, best decodable audio. It does not represent alternate tracks, subtitles,
attachments, alpha, or interlaced field cadence. Those remain available only in the original.

## Exact `.vepts` map

Variable-rate and unusual-origin media cannot be mapped safely through a proxy's nominal frame
rate. Every generated proxy therefore has a versioned binary `.vepts` sidecar containing:

- codec and container identifiers;
- the source fingerprint, size, modification time, quick hash, and optional full hash;
- source/proxy stream indexes and exact rational time bases;
- the source origin PTS;
- for each frame, source PTS/duration and proxy PTS/duration.

The fixed-width little-endian format starts with `VEPTSMAP` and version 1. Readers reject an unknown
version, invalid time bases, non-monotonic records, and trailing bytes. The map—not a nominal FPS—is
the contract for mapping a proxy frame back to original media.

The current playback registry can select a generated proxy, but its FFmpeg provider does not yet
consume the `.vepts` map for all seek decisions. Completing map-driven playback validation across
VFR and unusual PTS media remains a beta gate.

## Atomic generation and cancellation

Proxy generation writes unique temporary sibling files. A stop token interrupts work; cancellation
or failure removes incomplete temporary output and does not commit an invalid proxy/map pair.
Successful completion installs both outputs, verifies the result, updates the session's asset
manifest, registers the proxy for playback, and invalidates existing decoded state.

Desktop generation currently runs in-process with QtConcurrent. The worker executable also accepts
`JOB_KIND_PROXY` with unchanged V1 Protobuf fields and two preset IDs:

- `video-editor.proxy.prores-half.v1` requests the default half-resolution ProRes profile and allows
  the FFV1 fallback;
- `video-editor.proxy.ffv1-half.v1` requires the half-resolution FFV1 profile.

A proxy worker request requires exactly one absolute input and one absolute output; its map defaults
to `<output>.vepts`. Validation fails closed and events advance monotonically through the versioned
accepted/running/terminal states. The current stdin/stdout loop is synchronous, so it cannot consume
a `CancelJob` while transcode work is running and reports idle cancellation as unsupported. The
desktop is not routed through this worker or through the planned named-pipe/Unix-socket supervisor,
so restartable isolation and worker-death recovery remain incomplete.

## Playback selection

The thread-safe asset registry stores an original plus an optional proxy. When preview permits a
proxy and its file exists, it is selected; a missing proxy transparently falls back to the original.
Export constructs an independent provider and always requests proxies disabled. Proxies never
become delivery sources.

Playback owns persistent demux/decoder sessions. Nearby forward requests continue decoding; reverse
or distant requests seek to the preceding keyframe and decode forward in presentation order. A new
request epoch interrupts stale FFmpeg I/O and discards a poisoned session after decode failure.

Timeline audio has a separate originals-only registry whose type cannot represent a proxy. Its
offline renderer decodes/resamples requested immutable snapshot ranges to exact 48 kHz stereo planar
float blocks. This deliberately prevents proxy audio from becoming export authority. The reference
exporter consumes those blocks and muxes deterministic PCM; this is not yet an audio-device path.

The CPU color path uses libswscale with Rec.709 coefficients and converts to approximately
scene-linear float RGBA. It is not a full input color-management or HDR tone-mapping pipeline.

## Rebuildable artifacts: thumbnails, waveforms, metadata

The `media_cache` module owns rebuildable media artifacts that live outside
`.veproj`. A shared `CacheStore` is a content-addressed on-disk blob store: one
file per entry under `<root>/blobs/<sha256-of-key>`, with a SQLite index at
`<root>/index.sqlite` recording size and access time. Entries are keyed by
`(asset_id, kind, parameter_hash)` so a service can supersede an entry when its
generation parameters change without invalidating the whole asset. The store
enforces a configurable byte budget (default 100 GB) with least-recently-used
eviction; `put` writes a temp file, fsyncs it on POSIX, renames it into place,
and protects the just-inserted entry from eviction. `inspect()` returns an
LRU-ordered inventory for a future cache browser.

Three services fill the store:

- `thumbnail_service` extracts a deterministic JPEG still (First/Middle/Last
  frame, Middle = floor(duration/2)) scaled to the long edge preserving aspect
  ratio (no upscale, even dimensions). Pure resolvers are unit-tested; the
  FFmpeg decode/scale/encode path is cancellable via `std::stop_token`.
- `waveform_service` decodes the best audio stream, resamples to mono float32
  at 48 kHz, and builds a pyramid of `{min, max, rms}` buckets. Each level
  halves the bucket count; empty buckets use a sentinel (`min=1, max=-1`) so
  the UI can distinguish "no data" from "silence". Serialization uses a
  versioned `VEWAVE01` little-endian format.
- `metadata_service` stores a per-asset user-editable document (title, tags,
  notes, rating 0–5, ordered custom fields) distinct from the probed technical
  metadata in `media::AssetDescriptor`. Serialization uses `VEMETA01`.

Deleting the cache directory never destroys project edits or original media;
all artifacts are rebuilt on demand. The store is not thread-safe; callers
serialize through the owning service.

## Cache locations and current limits

The desktop places proxies below Qt's per-user cache location in a `proxies` directory, using the
asset ID in the file name. The corresponding `.vepts` sidecar uses the proxy path plus the sidecar
suffix. Working project recovery files are separate and live under application-local data, not the
cache.

Current limitations:

- generated proxy association is not persisted or rediscovered after reopen;
- the `media_cache` budget is not yet shared with proxy storage — each lives
  under its own root and is evicted independently. A unified budget across
  proxies, thumbnails, waveforms, and metadata remains future work;
- there is no cache browser UI, disk-full guard, or cleanup policy;
- `media_cache` FFmpeg-dependent generation paths are not yet covered by
  integration tests with generated media fixtures;
- unplugged/missing-media and disk-full behavior has not passed the required
  fault matrix.

Deleting the cache should never destroy project edits or original media, but users should close the
application before manual cleanup because an active proxy or preview job may be using it.

See [ADR 0007](../architecture/0007-proxy-pts-map.md) and the
[project-state boundary](project-format-and-recovery.md#stored-state).

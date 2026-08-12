<!-- SPDX-License-Identifier: MPL-2.0 -->

# Beta feature status

This is an implementation matrix, not a roadmap promise. Status is judged end to end from the
desktop workflow, not from the existence of a class or panel.

- **Implemented**: usable in the current desktop vertical slice and covered by automated tests.
- **Partial**: a meaningful contract or workflow exists, but required beta behavior is absent.
- **Missing**: no usable implementation is present.
- **Deferred**: intentionally outside public-beta scope.

## Current public-beta scope

| Area | Status | Current reality / remaining work |
| --- | --- | --- |
| Qt desktop shell and workspaces | Implemented | Dockable Import, Edit, Audio & Captions, Deliver layouts, command palette, accessible names, persistent presentation shell. Full accessibility/usability study still gates release. |
| Exact rational timeline and revisions | Implemented | Integer rational time, 128-bit intermediates, explicit rounding, expected revisions, immutable snapshots, undo/redo. Windows currently requires `clang-cl`. |
| Core insert/move/trim/split/delete | Implemented | Typed validated commands, normal/overwrite/ripple trim policy, explicit linked split IDs, linked move/trim/split/remove, exact gap close, atomic multi-command revisions, and one-step undo/redo. |
| Interactive timeline gestures | Implemented | Replace/toggle/range multi-selection; transient preview/single commit/Escape/autoscroll; move and normal/ripple/overwrite trims; roll/slip/slide tools; exact resolver-backed playhead/marker/clip/frame snapping; exact frame-count nudging; track, marker, and gap interaction. |
| Precision edit model | Implemented | Roll, slip, slide, linked A/V semantics, atomic batches, exact NTSC frame operations, snap exclusions/ties, overwrite/ripple trims, track state, and derived-gap closure are tested through model and desktop/controller boundaries. |
| Track-based editing breadth | Partial | Unlimited model vectors plus desktop create/rename/reorder/remove, lock, visual visibility, targeting, markers, and derived-gap interaction are implemented. Source-monitor insert/overwrite editing, title/transition authoring, speed controls, track audio gain/pan, and large-track release performance evidence remain incomplete. |
| Media import and probing | Implemented | Async file/dialog/drop import, fingerprints, FFmpeg descriptors, searchable media bin, linked A/V insertion. Still-image/image-sequence breadth and the 200-file matrix are unproven. |
| Missing media and relink | Partial | Asset service can detect/relink and the UI shows offline/relink affordances, but application relink is not connected and imported records are not fully reconstructed from checkpoints. |
| Metadata, thumbnails, waveforms | Partial | A shared content-addressed `media_cache` store (SQLite index + per-entry blob files, 100 GB LRU budget, atomic put, `inspect()` inventory) backs three services: `thumbnail_service` (deterministic First/Middle/Last frame → MJPEG via FFmpeg, cancellable), `waveform_service` (mono float32 pyramid of min/max/rms buckets with versioned `VEWAVE01` serialization), and `metadata_service` (per-asset editable document with `VEMETA01` serialization). Pure resolvers and serializers are unit-tested; FFmpeg-dependent generation and desktop UI wiring (media-bin thumbnails, clip-header waveforms, metadata editor panel) remain. |
| Proxy generation | Partial | UI-triggered, cancellable half-resolution ProRes/PCM or FFV1 fallback; exact `.vepts`; session playback. A worker implements validated ProRes-half/FFV1-half jobs, but desktop still runs in-process. No automatic queue, persistence/discovery, IPC routing, or cache budget. |
| CPU video preview | Implemented | Persistent FFmpeg sessions, keyframe seek, request epochs, immutable snapshot rendering, original/proxy selection, CPU Rec.709 approximation. |
| GPU preview and color management | Partial | A pinned, capability-gated libplacebo backend provides D3D11 on Windows and Vulkan on Linux, truthful unavailable stubs, device-loss state, CPU fallback, and per-clip GPU preview for active video clips with crop/position/scale/anchor, centered-pivot rotation, opacity, and Normal composition. Rotation with a moved pivot, effects, titles, and other blends fall back only for that frame. The desktop requests GPU before CPU and downloads the offscreen result for Qt; backend/device failures preserve that frame's CPU result and latch CPU preview for the session. Device creation is still synchronous during controller startup. Asynchronous initialization, native swapchain presentation, effect/color parity, YUV/native imports, hardware decode/encode, zero-copy, HDR tone mapping, adaptive quality, and the release hardware matrix remain missing. |
| Transform and blend properties | Partial | Position X/Y, uniform and separate Scale X/Y, Anchor X/Y, Crop edges, Rotation, Opacity and five blend modes are revisioned, undoable, persistent, and CPU-rendered. Signed scale flips lack Inspector input; keyframes and most effects remain unwired/unrendered. |
| Titles, transitions, speed/reverse | Partial | Canonical title payloads and sequence-owned transitions are revisioned edit-model state, persist through schema-v2 snapshots and project-store migration, and render in the CPU reference path. Active titles/transitions return typed GPU fallback so preview stays exact. `SetClipSpeedCommand` (0.01x–100x + reverse) is revisioned and validated. Desktop authoring is wired: Inspector title controls (text, font, size, alignment, bold/italic), speed/reverse controls, and keyframe-toggle forwarding are connected to `SetClipTitleCommand`/`SetClipSpeedCommand`/`SetClipEffectParameterCommand`; the timeline paints transitions with drag-handle duration editing, preset switching, and removal. A full keyframe curve editor, native GPU title/transition shaders, and reverse-preview polish remain. |
| Rec.709 color controls and LUTs | Missing | CPU conversion/export approximation exists, but exposure, WB, tint, contrast, saturation, curves, LUT import, metadata-preserving transforms, and HDR-to-SDR pipeline are not integrated. |
| Timeline audio render and clip properties | Partial | Originals-only decode/resample renders exact half-open 48 kHz stereo blocks with gaps/overlaps, gain/pan/fades, mute/solo, rate/reverse and cancellation; reference export consumes it. Forward 1× desktop playback uses an immutable snapshot, bounded pre-render ring, optional pinned miniaudio output, and a latency-compensated audio master clock with submitted-position/uncertainty diagnostics. It has explicit silent timer fallback and underrun diagnostics. `AsyncRealtimeAudioPlayback` provides GUI-safe serialized, versioned controls; the controller holds video during pending start/seek and becomes audio-master only after effective success. Reverse/non-1× audio, effect/master DSP, adaptive buffering, latency calibration, and release device/drift validation remain missing. |
| Mixer, DSP, meters, normalization | Partial | Desktop M/S controls are revisioned, persistent, undoable, and consumed by audio render. Track faders are intentionally disabled. Track gain/pan, EQ/compressor/dialogue reduction/limiter, live peak/RMS/LUFS meters, and normalization are missing. |
| SRT/WebVTT captions | Implemented | Deterministic import/export, validation, transcript search, add/text edit/delete, and atomic subtitle output. Timing fields and styling UI are limited. |
| Local transcription | Missing | Worker message kind and explanatory UI exist; no whisper.cpp worker, checksummed model manager, word timing, Vulkan path, or VAD workflow. |
| Caption reflow and proposals | Partial | Dependency-light reflow/search APIs exist. No review UI, silence detector, transcript edit proposals, or ordinary-command apply workflow. |
| Caption render/burn-in | Partial | A deterministic bitmap-glyph caption burn-in renderer (`export_service/caption_burn_in.h`) draws `edit::Caption` text onto composited `CpuFrame`s before YUV conversion, sharing the 5x7 glyph set with the title rasterizer. SRT/WebVTT sidecar export reuses `caption_service` serialization with nearest-millisecond timestamps. Burn-in is wired into `export_service.cpp` via `ExportRequest::caption_mode`. Style preview in the caption panel, richer font support, and embedded caption tracks remain missing. |
| Safe reference export | Implemented | Immutable revision, originals only, exact video frames plus `ceil(duration × 48000)` stereo PCM S16LE samples in 960-sample packets, cancellation, typed audio errors, and atomic commit. FFV1/MKV or encoder-gated ProRes/MOV. |
| Creator delivery export | Partial | Platform presets (YouTube 1080p/1440p/4K, vertical 9:16, podcast audio-only), `ExportRequest` controls (resolution/frame-rate/audio-bitrate overrides, caption mode, sidecar format), and a hardware-encoder capability matrix (NVENC/QSV/AMF/VideoToolbox + software probing) are implemented. Caption burn-in and SRT/WebVTT sidecar export work with the existing FFV1/ProRes reference path. The H.264/AAC delivery codec path is held pending legal/patent review (`VIDEO_EDITOR_H264_DELIVERY_APPROVED` / `VIDEO_EDITOR_AAC_DELIVERY_APPROVED` CMake gates, both OFF). `DeliverPanelWidget` UI expansion, resolution scaling in the encoder, and loudness targeting remain. |
| Project checkpoints and journaling | Implemented | SQLite WAL/FULL local database, versioned snapshot journal, online-backup checkpoint, integrity validation, fsync and atomic replace. |
| Startup recovery | Implemented | Read-only bounded catalog, diagnostics for invalid candidates, newest recommended recovery prompt, latest committed snapshot. Broader fault-injection matrix remains a release gate. |
| Schema migration | Partial | Forward transactional migration, pre-migration backups, schema-v2 payload-version journaling, backward read of schema-v1 snapshots, and recovery tests for v1/v2 candidates are implemented. Broader beta history still does not exist yet, so multi-version upgrade claims remain limited. |
| Background job protocol | Partial | Protobuf framing, IDs, cancellation registry, probe jobs, and validated proxy preset jobs work. The synchronous worker cannot receive cancel while proxying; desktop IPC/process routing, export/transcription workers, named pipes, and Unix sockets remain missing. |
| Cache management | Partial | A content-addressed `media_cache` store provides a 100 GB configurable budget, dependency-hash keys, disk LRU eviction, atomic writes, and an `inspect()` inventory for thumbnails/waveforms/metadata. Cross-module budget sharing with proxies, a cache browser UI, and eviction-on-disk-full fault handling remain. |
| Accessibility and shortcuts | Partial | Accessible labels/focus, keyboard navigation, workspaces, searchable commands, professional tool shortcuts, and keyboard track/marker/gap operations exist. Remapping, a platform screen-reader audit, and beginner study remain. |
| Windows/Linux packaging | Partial | WiX MSI and Flatpak skeletons exist. Runtime bundles, immutable sources, signing, identity, linting, clean-machine tests, and notices/source offers are incomplete. |
| CI and corpus | Partial | CPU workflows, sanitizer workflow, quality scripts, schema checks, and module tests exist. Windows/GPU/codec matrices, nightly/weekly endurance, and 200+ licensed media files are incomplete. |
| H.264/AAC legal distribution | Missing | Requires approved encoder/runtime configuration and specialist codec/patent review before public beta. |

## Explicitly deferred until after beta

| Capability | Status |
| --- | --- |
| Nested/compound timelines and multicam | Deferred |
| Stabilization, tracking, and video scopes | Deferred |
| Arbitrary audio buses, surround delivery, HDR mastering | Deferred |
| Motion compositing and render queues | Deferred |
| OTIO/XML/EDL interchange and public plugins | Deferred |
| Collaboration, accounts, cloud sync, and hosted features | Deferred |
| macOS, ARM64, and generative media | Deferred |

## Critical path to public beta

The dominant product blockers are release-matrix realtime audio/xrun/drift hardening and creator
delivery codecs; effects/color/title authoring through the render graph; native GPU presentation,
effects/color parity, and full hardware/fallback testing; restartable workers and local transcription;
persistent media/cache management;
and the packaging, accessibility, codec/GPU corpus, endurance, security, and legal gates.

Until those are complete, use the application as a tested engineering preview and reference
implementation rather than as a production editor.

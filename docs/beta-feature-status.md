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
| Track-based editing breadth | Partial | Unlimited model vectors plus desktop create/rename/reorder/remove, lock, visual visibility, targeting, markers, derived-gap interaction, title/transition authoring, speed controls, and track gain/pan are implemented. Source-monitor insert/overwrite editing and large-track release performance evidence remain incomplete. |
| Media import and probing | Implemented | Async file/dialog/drop import, fingerprints, FFmpeg descriptors, searchable media bin, linked A/V insertion. Still-image/image-sequence breadth and the 200-file matrix are unproven. |
| Missing media and relink | Partial | Asset service can detect/relink and the UI shows offline/relink affordances, but application relink is not connected and imported records are not fully reconstructed from checkpoints. |
| Metadata, thumbnails, waveforms | Partial | A shared content-addressed `media_cache` store (SQLite index + per-entry blob files, 100 GB LRU budget, atomic put, `inspect()` inventory) backs three services: `thumbnail_service` (deterministic First/Middle/Last frame → MJPEG via FFmpeg, cancellable), `waveform_service` (mono float32 pyramid of min/max/rms buckets with versioned `VEWAVE01` serialization), and `metadata_service` (per-asset editable document with `VEMETA01` serialization). Pure resolvers and serializers are unit-tested; FFmpeg-dependent generation and desktop UI wiring (media-bin thumbnails, clip-header waveforms, metadata editor panel) remain. |
| Proxy generation | Partial | UI-triggered, cancellable half-resolution ProRes/PCM or FFV1 fallback; exact `.vepts`; session playback. A worker implements validated ProRes-half/FFV1-half jobs, but desktop still runs in-process. No automatic queue, persistence/discovery, IPC routing, or cache budget. |
| CPU video preview | Implemented | Persistent FFmpeg sessions, keyframe seek, request epochs, immutable snapshot rendering, original/proxy selection, CPU Rec.709 approximation. |
| GPU preview and color management | Partial | A pinned, capability-gated libplacebo backend provides D3D11 on Windows and Vulkan on Linux, truthful unavailable stubs, device-loss state, CPU fallback, and per-clip GPU preview for active video clips with crop/position/scale/anchor, centered-pivot rotation, opacity, and Normal composition. Rotation with a moved pivot, effects, titles, and other blends fall back only for that frame. The desktop requests GPU before CPU and downloads the offscreen result for Qt; backend/device failures preserve that frame's CPU result and latch CPU preview for the session. Device creation is still synchronous during controller startup. Asynchronous initialization, native swapchain presentation, effect/color parity, YUV/native imports, hardware decode/encode, zero-copy, HDR tone mapping, adaptive quality, and the release hardware matrix remain missing. |
| Transform, blend, effects, and animation | Partial | Transform/crop/opacity and five blend modes are revisioned and CPU-rendered. The Effects panel creates typed color, crop, and blur effects; the Inspector exposes current values plus clip-local Hold/Linear/Bezier keyframe lists, exact fields, deletion, and a curve editor. CPU preview/export share the evaluator and expensive blur is bypassed only when its preview profile allows it; active effects fall back per frame from GPU to CPU. Signed-scale Inspector input, LUT import, a broader effect set, and native GPU effect shaders remain. |
| Titles, transitions, speed/reverse | Implemented | Title creation and Inspector text/font/size/alignment/emphasis controls, transition handles/presets/duration/removal, and 0.01x–100x/reverse controls are revisioned, undoable, schema-v3 persistent, and CPU preview/export rendered. The GPU path returns typed per-frame fallback for titles and transitions. Native GPU shaders and higher-quality pitch-preserving retime remain later optimizations, not separate authoring models. |
| Rec.709 color controls and LUTs | Partial | Known CPU effects expose exposure, temperature/tint, contrast, and saturation through the generic effect/keyframe UI. White-balance sampling, dedicated color curves, LUT import, metadata-preserving transforms, HDR-to-SDR completeness, and GPU parity remain missing. |
| Timeline audio render and clip properties | Partial | Originals-only decode/resample renders exact half-open 48 kHz stereo blocks with gaps/overlaps, clip and track gain/pan, fades, mute/solo, rate/reverse, ordered track DSP, and cancellation; export and realtime pre-render consume it. Forward 1× playback uses a bounded ring, selected miniaudio device, latency-compensated audio-master clock, serialized versioned controls, explicit silent fallback, underrun diagnostics, and one-second off-UI-thread selected/default-device loss/recovery polling. Reverse/non-1× realtime audio, calibrated hardware timestamps, native event-driven hot-plug notification, and physical release-matrix endurance remain missing. |
| Mixer, DSP, meters, normalization | Partial | The Audio workspace exposes revisioned track gain/pan/mute/solo and current-value controls for parametric EQ, compressor, dialogue noise reduction, and limiter. Audio render runs the ordered stateful chain. Sample-range/stable-ID track peak/RMS follows the audio master; master peak/RMS and worker-owned EBU-R128 `LUFS-I` expose valid/analyzing/stale states. Background EBU-R128 analysis creates a reviewable revision/target-generation-bound change set for an editable −24 through −9 LUFS target, rejects unsafe gain, and applies one atomic batch. Device selection and polling/recovery are wired. Accelerated one-hour zero-xrun and two-hour less-than-one-frame drift simulations pass; physical-device matrix evidence and the stricter 10 ms gate remain release requirements. |
| SRT/WebVTT captions | Implemented | Deterministic import/export, validation, timed-word transcript search/navigation, add/text edit/delete, canonical style controls, and atomic subtitle output. |
| Local transcription | Partial | The desktop explicitly downloads and verifies the pinned multilingual base model, then launches a one-job worker that seeks and decodes the selected authoritative source range to mono 16 kHz float32 and returns validated, source-absolute word timing through the framed Protobuf v2 contract. `whisper.cpp` 1.9.2 is an optional exact-pinned build capability with a truthful compiled Vulkan capability; builds without it report `BackendUnavailable`. Physical multilingual/Vulkan inference and worker-death matrix evidence remain release work. |
| Caption reflow and proposals | Implemented | Exact timed-word reflow/search, immutable 48 kHz measured-silence analysis, conservative unchecked filler-word proposals, per-item review, stale/cancel gates, deterministic range merging, and one atomic revision/undo step for accepted captions and cuts are connected. |
| Caption render/burn-in | Partial | Canonical alignment, normalized vertical position, safe margin, emphasis, colors, and outline are shared by the caption panel, schema-v3 project state, and deterministic bitmap burn-in before YUV conversion. Sidecars use atomic SRT/WebVTT serialization. Rich font shaping/family parity and embedded caption streams remain missing. |
| Safe reference export | Implemented | Immutable revision, originals only, exact video frames plus `ceil(duration × 48000)` stereo PCM S16LE samples in 960-sample packets, cancellation, typed audio errors, and atomic commit. FFV1/MKV or encoder-gated ProRes/MOV. |
| Creator delivery export | Partial | The Deliver workspace produces VP9/Opus WebM through FOSS `libvpx-vp9`/`libopus` presets for YouTube 1080p/1440p/4K, vertical 9:16, and Opus-only podcast output. Resolution, exact frame rate, bitrate/VP9 quality, audio bitrate, caption burn-in/sidecar, aspect-preserving scale/letterbox, originals-only render, and atomic cancellation safety are wired. Lightweight encoder discovery keeps the panel responsive; the export worker can select VP9 QSV on Windows or VP9 VAAPI on Linux and validates the device there. Device/frame/upload/encode failures discard the attempt and restart deterministically with libvpx before commit. H.264/AAC remain disabled and were explicitly excluded from this implementation pending legal review. |
| Project checkpoints and journaling | Implemented | SQLite WAL/FULL local database, versioned snapshot journal, online-backup checkpoint, integrity validation, fsync and atomic replace. |
| Startup recovery | Implemented | Read-only bounded catalog, diagnostics for invalid candidates, newest recommended recovery prompt, latest committed snapshot. Broader fault-injection matrix remains a release gate. |
| Schema migration | Partial | The SQLite envelope is schema v2 with forward transactional migration, pre-migration backup, and payload-version journaling. Snapshot schema v3 writes timed captions while strictly reading v1/v2 defaults; recovery tests cover old store/snapshot candidates. Broader beta history still does not exist yet, so multi-version upgrade claims remain limited. |
| Background job protocol | Partial | Protobuf framing, IDs, cancellation registry, probe/proxy jobs, and typed transcription request/progress/result events work. The desktop launches a restartable one-job transcription worker over framed standard I/O and cancellation terminates that process. Proxy/export still run in process, the synchronous proxy worker cannot consume cancel while transcoding, and named-pipe/Unix-socket routing is not implemented. |
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

The dominant product blockers are physical release-matrix realtime audio/xrun/drift and native
hot-plug/latency calibration; approved H.264/AAC; full color/LUT breadth; native GPU
presentation, effects/color parity, and full hardware/fallback testing; physical multilingual/Vulkan
transcription validation plus restartable proxy/export workers; persistent media/cache management;
and the packaging, accessibility, codec/GPU corpus, endurance, security, and legal gates.

Until those are complete, use the application as a tested engineering preview and reference
implementation rather than as a production editor.

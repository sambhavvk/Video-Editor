<!-- SPDX-License-Identifier: MPL-2.0 -->

# Beta feature status

This is an implementation matrix, not a roadmap promise. Status is judged end to end from the
desktop workflow, not from the existence of a class or panel.

The **first public beta is Linux exclusive**. Windows distribution and the Windows GPU/codec matrix
are deferred; see below.

- **Implemented**: usable in the current desktop vertical slice and covered by automated tests.
- **Partial**: a meaningful contract or workflow exists, but required beta behavior is absent.
- **Missing**: no usable implementation is present.
- **Deferred**: intentionally outside the current public-beta scope (including Windows-only
  packaging and GPU matrix work).

## First public beta: Linux exclusive

The first public beta is **Linux x86-64 only** (source builds on current glibc distributions, plus
the Flatpak packaging path). Windows remains an engineering preview and is not a first-beta
distribution target.

Signed Windows MSI identity, runtime bundling, Authenticode, and the Windows Intel/AMD/NVIDIA
D3D11/GPU/codec matrix are **deferred**. Research on those Windows packaging and GPU compatibility
paths showed they need more calendar time than the Linux beta. A later Windows beta will pick them
up; they are not first-beta gates.

Linux first-beta packaging is Flatpak. Store identity, immutable checksummed sources, and
`flatpak-builder-lint`/AppStream completeness remain Linux packaging work, not Windows work.

## Current public-beta scope

| Area | Status | Current reality / remaining work |
| --- | --- | --- |
| Qt desktop shell and workspaces | Implemented | Dockable Import, Edit, Audio & Captions, Deliver layouts, command palette, accessible names, persistent presentation shell. Full accessibility/usability study still gates release. |
| Exact rational timeline and revisions | Implemented | Integer rational time, 128-bit intermediates, explicit rounding, expected revisions, immutable snapshots, undo/redo. Windows currently requires `clang-cl`. |
| Core insert/move/trim/split/delete | Implemented | Typed validated commands, normal/overwrite/ripple trim policy, explicit linked split IDs, linked move/trim/split/remove, exact gap close, atomic multi-command revisions, and one-step undo/redo. |
| Interactive timeline gestures | Implemented | Replace/toggle/range multi-selection; transient preview/single commit/Escape/autoscroll; move and normal/ripple/overwrite trims; roll/slip/slide tools; exact resolver-backed playhead/marker/clip/frame snapping; exact frame-count nudging; track, marker, and gap interaction. |
| Precision edit model | Implemented | Roll, slip, slide, linked A/V semantics, atomic batches, exact NTSC frame operations, snap exclusions/ties, overwrite/ripple trims, track state, and derived-gap closure are tested through model and desktop/controller boundaries. |
| Track-based editing breadth | Partial | Unlimited model vectors plus desktop create/rename/reorder/remove, lock, visual visibility, targeting, markers, derived-gap interaction, title/transition authoring, speed controls, and track gain/pan are implemented. Source-monitor insert/overwrite editing and large-track release performance evidence remain incomplete. |
| Media import and probing | Implemented | Async file/dialog/drop import, fingerprints, FFmpeg descriptors, searchable media bin, linked A/V insertion. Still-image/image-sequence breadth remains. A synthetic 200+ lavfi corpus generator covers required categories; a physical decode lab is still a gate. |
| Missing media and relink | Implemented | Opening a project rebuilds session `AssetRecord`s, marks Online/Missing/Changed, auto-recovers matching files from the project folder, last relink directory, or original parent, and connects media-bin Relink (with a confirm when content changed). `RelinkAssetCommand` persists the new URI. Automated save/checkpoint and cache disk-full injection exist; physical unplug remains a hardware-lab check. |
| Metadata, thumbnails, waveforms | Implemented | The desktop owns a `CacheStore`, generates thumbnails/waveforms serially on import and reopen, shows media-bin JPEG thumbnails and timeline min/max waveforms, and edits per-asset title/tags/notes/rating in the Inspector. FFmpeg generation is covered by a lavfi fixture test. |
| Proxy generation | Implemented | Recommended 4K long-GOP media is auto-queued one job at a time; manual Create editing proxy remains. Completed proxies and `.vepts` maps are adopted into `CacheStore` and rediscovered on reopen (legacy `{assetId}.proxy.*` is a read-only fallback). Playback seeks proxies through the registered `.vepts` map and falls back to originals when the map is missing or invalid. The desktop launches a fresh `JOB_KIND_PROXY` worker-host process; kill cancels the job and worker death does not register a complete proxy. Duplex in-flight `CancelJob` and VFR corpus proof remain. |
| CPU video preview | Implemented | Persistent FFmpeg sessions, keyframe seek, request epochs, immutable snapshot rendering, original/proxy selection with `.vepts`-mapped proxy seek, in-memory LRU preview cache keyed by revision/time/profile/proxy generation, CPU Rec.709 approximation. |
| GPU preview and color management | Partial | A pinned, capability-gated libplacebo backend provides Vulkan on Linux and D3D11 on Windows, truthful unavailable stubs, device-loss state, CPU fallback, and per-clip GPU preview for active video clips with crop/position/scale/anchor, centered-pivot rotation, opacity, and Normal composition. Rotation with a moved pivot, effects, titles, and other blends fall back only for that frame. The desktop requests GPU before CPU and downloads the offscreen result for Qt; backend/device failures preserve that frame's CPU result and latch CPU preview for the session. Device creation runs off the UI thread after CPU preview is already available. Native swapchain presentation, effect/color parity, YUV/native imports, hardware decode/encode, zero-copy, HDR tone mapping, and adaptive quality remain missing. The **first-beta hardware matrix is Linux Vulkan** (Intel/AMD/NVIDIA). The Windows D3D11 GPU matrix is deferred with the Windows beta. |
| Transform, blend, effects, and animation | Partial | Transform/crop/opacity and five blend modes are revisioned and CPU-rendered. The Effects panel creates typed color, crop, and blur effects (clip audio DSP is mixer-only); the Inspector exposes signed scale, current values plus clip-local Hold/Linear/Bezier keyframe lists, exact fields, deletion, and a curve editor. CPU preview/export share the evaluator and expensive blur is bypassed only when its preview profile allows it; active effects fall back per frame from GPU to CPU. LUT import, a broader effect set, and native GPU effect shaders remain. |
| Titles, transitions, speed/reverse | Implemented | Title creation and Inspector text/font/size/alignment/emphasis controls, transition handles/presets/duration/removal, and 0.01x–100x/reverse controls are revisioned, undoable, schema-v3 persistent, and CPU preview/export rendered. The GPU path returns typed per-frame fallback for titles and transitions. Native GPU shaders and higher-quality pitch-preserving retime remain later optimizations, not separate authoring models. |
| Rec.709 color controls and LUTs | Partial | Known CPU effects expose exposure, temperature/tint, contrast, and saturation through the generic effect/keyframe UI. White-balance sampling, dedicated color curves, LUT import, metadata-preserving transforms, HDR-to-SDR completeness, and GPU parity remain missing. |
| Timeline audio render and clip properties | Partial | Originals-only decode/resample renders exact half-open 48 kHz stereo blocks with gaps/overlaps, clip and track gain/pan, fades, mute/solo, rate/reverse, ordered track DSP, and cancellation; export and realtime pre-render consume it. Forward 1× playback uses a bounded ring, selected miniaudio device, latency-compensated audio-master clock, serialized versioned controls, explicit silent fallback, underrun diagnostics, and one-second off-UI-thread selected/default-device loss/recovery polling. Reverse/non-1× realtime audio, calibrated hardware timestamps, native event-driven hot-plug notification, and physical release-matrix endurance remain missing. |
| Mixer, DSP, meters, normalization | Partial | The Audio workspace exposes revisioned track gain/pan/mute/solo and current-value controls for parametric EQ, compressor, dialogue noise reduction, and limiter. Audio render runs the ordered stateful chain. Sample-range/stable-ID track peak/RMS follows the audio master; master peak/RMS and worker-owned EBU-R128 `LUFS-I` expose valid/analyzing/stale states. Background EBU-R128 analysis creates a reviewable revision/target-generation-bound change set for an editable −24 through −9 LUFS target, rejects unsafe gain, and applies one atomic batch. Device selection and polling/recovery are wired. Accelerated one-hour zero-xrun and two-hour less-than-one-frame drift simulations pass; physical-device matrix evidence and the stricter 10 ms gate remain release requirements. |
| SRT/WebVTT captions | Implemented | Deterministic import/export, validation, timed-word transcript search/navigation, add/text edit/delete, canonical style controls, and atomic subtitle output. |
| Local transcription | Partial | The desktop explicitly downloads and verifies the pinned multilingual base model, then launches a one-job worker that seeks and decodes the selected authoritative source range to mono 16 kHz float32 and returns validated, source-absolute word timing through the framed Protobuf v2 contract. `whisper.cpp` 1.9.2 is an optional exact-pinned build capability with a truthful compiled Vulkan capability; builds without it report `BackendUnavailable`. Worker-host SIGKILL and stub-exit tests prove death does not mutate the project. Physical multilingual/Vulkan inference remains a lab check. |
| Caption reflow and proposals | Implemented | Exact timed-word reflow/search, immutable 48 kHz measured-silence analysis, conservative unchecked filler-word proposals, per-item review, stale/cancel gates, deterministic range merging, and one atomic revision/undo step for accepted captions and cuts are connected. Timeline cut proposals remap or drop transitions that shift or overlap cuts instead of failing the whole proposal. |
| Caption render/burn-in | Partial | Canonical alignment, normalized vertical position, safe margin, emphasis, colors, and outline are shared by the caption panel, schema-v3 project state, and deterministic bitmap burn-in before YUV conversion. Sidecars use atomic SRT/WebVTT serialization. Rich font shaping/family parity and embedded caption streams remain missing. |
| Safe reference export | Implemented | Immutable revision, originals only, exact video frames plus `ceil(duration × 48000)` stereo PCM S16LE samples in 960-sample packets, cancellation, typed audio errors, and atomic commit. FFV1/MKV or encoder-gated ProRes/MOV. |
| Creator delivery export | Partial | The Deliver workspace produces VP9/Opus WebM through FOSS `libvpx-vp9`/`libopus` presets for YouTube 1080p/1440p/4K, vertical 9:16, and Opus-only podcast output. Resolution, exact frame rate, bitrate/VP9 quality, audio bitrate, caption burn-in/sidecar, aspect-preserving scale/letterbox, originals-only render, and atomic cancellation safety are wired. The desktop serializes a project snapshot and launches `JOB_KIND_EXPORT` on a fresh worker host; kill leaves the destination unchanged. Lightweight encoder discovery keeps the panel responsive; the export worker can select VP9 VAAPI on Linux (QSV on Windows preview) and validates the device there. Device/frame/upload/encode failures discard the attempt and restart deterministically with libvpx before commit. H.264/AAC remain disabled pending legal review. |
| Project checkpoints and journaling | Implemented | SQLite WAL/FULL local database, versioned snapshot journal, online-backup checkpoint, integrity validation, fsync and atomic replace. |
| Startup recovery | Implemented | Read-only bounded catalog, diagnostics for invalid candidates, newest recommended recovery prompt, latest committed snapshot. Checkpoint-fault tests cover missing parent, parent-is-a-file, and read-only overwrite without replacing a good `.veproj`. Physical process-kill of the UI during save remains a lab check. |
| Schema migration | Partial | The SQLite envelope is schema v2 with forward transactional migration, pre-migration backup, and payload-version journaling. Snapshot schema v3 writes timed captions while strictly reading v1/v2 defaults; recovery tests cover old store/snapshot candidates. Broader beta history still does not exist yet, so multi-version upgrade claims remain limited. |
| Background job protocol | Partial | Protobuf framing, IDs, cancellation registry, probe/proxy/export/transcribe jobs, and typed transcription/export options work. The desktop launches a restartable one-job worker host over framed standard I/O for proxy, export, and transcription; cancellation and worker death terminate that process without committing a partial destination. The synchronous stdin/stdout loop cannot consume `CancelJob` while transcoding, and named-pipe/Unix-socket routing is not implemented. |
| Cache management | Implemented | One `CacheStore` budget (default 100 GB, persisted in `QSettings`) covers thumbnails, waveforms, metadata, proxies, and PTS maps. `File > Manage Media Cache…` inspects LRU inventory, changes the budget, removes entries/assets, evicts to budget, and clears artifacts without touching `.veproj` or originals. `CacheErrorCode::Full` stops remaining cache jobs. Oversized `put_file` injection proves Full leaves originals untouched; read-only checkpoint injection proves a failed save does not replace a good `.veproj`. |
| Accessibility and shortcuts | Partial | Accessible labels/focus, keyboard navigation, workspaces, searchable commands, professional tool shortcuts, and keyboard track/marker/gap operations exist. Offscreen tests walk interactive controls for empty accessible names, keep J/K/L/Space/Ctrl+1–4 bound, and cover a labeled Import → Edit → Audio & Captions → Deliver beginner path without a full encode. Remapping, a Linux screen-reader/AT-SPI audit, and the timed human beginner study remain. |
| Linux packaging (Flatpak) | Partial | The Flatpak skeleton is the first-beta packaging path. `tools/quality/validate_flatpak.py` checks no-network finish-args, desktop/metainfo/svg, and the source lock; unpinned sources and `releaseBlocking` remain warnings until `--store`. Runtime bundles, immutable checksummed sources, owned application ID/homepage, linting, AppStream, and clean-machine tests remain. |
| Windows packaging (MSI) | Deferred | WiX MSI remains a development skeleton. Signed MSI, runtime DLL harvest, identity, and clean-machine install/upgrade/uninstall are deferred until after the Linux-first public beta. |
| CI and corpus | Partial | Linux CPU/sanitizer workflows, quality scripts, schema checks, and module tests exist. A synthetic 200+ lavfi corpus generator and `--release` verifier cover required categories; binaries stay gitignored. `linux_capability_matrix.py` plus a weekly workflow probe Linux Vulkan/ffmpeg without failing when no GPU is present. Physical decode-lab and Windows GPU/codec matrices remain; Windows nightly endurance is deferred. |
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
| Signed Windows MSI and Windows GPU/codec matrix | Deferred until after the Linux-first public beta |

## Critical path to the Linux-first public beta

Windows MSI signing and the Windows GPU/codec matrix are **not** first-beta blockers.

The remaining Linux-first product blockers are physical Linux realtime audio/xrun/drift and native
hot-plug/latency calibration; approved H.264/AAC; full color/LUT breadth; native GPU presentation,
effects/color parity, and the Linux Vulkan hardware-lab matrix; physical multilingual/Vulkan
transcription validation; Flatpak identity, checksummed sources, and store lint; the timed human
accessibility/beginner study; endurance; security; and legal gates. Restartable proxy/export/
transcription workers, automated save/disk-full/GPU-loss/worker-death injection, and a synthetic
200+ corpus generator are in the tree.

Until those Linux gates are complete, use the application as a tested engineering preview and
reference implementation rather than as a production editor. Windows testers may still build from
source, but that is not the first public beta.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# Testing, quality, and release gates

The **first public beta is Linux x86-64**. Windows remains an engineering preview: signed MSI
identity, runtime bundling, and the Windows GPU/codec matrix are deferred because those packaging
and GPU compatibility paths need more calendar time. Passing the current unit and integration suite
means the engineering slice is internally consistent; it does not satisfy the Linux public-beta
acceptance gates by itself.

## Local checks

For a full configured development tree:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
tools/quality/format_changed_cpp.sh --check
tools/quality/dependency_license_gate.sh --source-only
python3 tools/quality/verify_corpus.py
python3 tools/quality/validate_flatpak.py
python3 tools/quality/linux_capability_matrix.py
cmake --build build/dev --target sbom
```

Generate the 200+ synthetic corpus before a release-style check (requires ffmpeg; binaries stay gitignored):

```sh
python3 tools/quality/generate_corpus.py
python3 tools/quality/verify_corpus.py --release
```

For memory and undefined-behavior checks on a supported non-MSVC toolchain:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

`core-only` is useful for deterministic edit/persistence work without desktop or media packages.
It is not a replacement for the full media and Qt suite.

## Present automated coverage

The repository currently includes tests for:

- rational time/property boundaries, entity IDs, edit invariants, revisions, single-command and
  atomic-batch undo/redo/failure, insert/move/normal-ripple-overwrite trim, explicit linked split,
  linked delete, roll/slip/slide, track rename/reorder/lock/visibility/targeting, exact derived-gap
  close/stale rejection, NTSC frame math, snap clip/marker exclusions and stable ties, validated
  clip properties, audio-track mute/solo/gain/pan/effects, canonical titles, transition invariants,
  typed effect validation, and Hold/Linear/Bezier curve evaluation;
- deterministic schema-v3 snapshot serialization, strict validation, schema-v1/v2 backward reads,
  older-field smuggling rejection (including additive track and caption fields), old-v2 caption and
  track defaults, timed-word/provenance/style round trips, track-state/title/transition round trips,
  and unknown-effect preservation;
- SQLite schema, revision conflicts, checkpoint cycles, transactional v1→v2 migration with validated
  pre-migration backup, failed-migration rollback, v1/v2 recovery discovery, recovery status, catalog
  sorting, corrupt candidates, and candidate bounds;
- FFmpeg probing, random/exact CPU frame requests, cancellation epochs, asset registry behavior,
  CPU transform/crop/opacity/blend and invisible-track behavior, deterministic title glyphs, cross-dissolve and
  dip-to-black transition boundaries, source-handle mapping, provider-failure propagation, request
  epochs and cache keys, deterministic video plus PCM export, VP9/Opus WebM creator output,
  aspect-preserving scale/letterbox, exact output-rate frame counts, podcast audio-only topology,
  styled caption burn-in/sidecars including alignment, vertical position, safe margin, outline and
  legacy defaults, exact decode-back frame/sample spans, and cancellation safety;
- pure GPU backend selection and unavailable-stub diagnostics, plus real Vulkan
  create/upload/normal-composite/download parity, per-clip GPU transform/composition behavior, and
  device-loss state when the exact dependency and a permitted device are available. Title and
  transition frames return typed `GpuUnsupportedTimeline` results, and desktop tests verify CPU
  fallback without permanently disabling the GPU; an opt-in desktop smoke check asserts that the
  per-clip GPU preview becomes active;
- proxy profile resolution, PTS-map validation/round-trip, transcode behavior, cancellation, and
  destination safety;
- subtitle validation/round-trip, timed-word reflow/search/navigation, deterministic caption
  change sets, exact 48 kHz silence boundaries, linked timeline-cut proposals, stale/no-op/locked
  rejection, and atomic caption-plus-cut apply/undo workflows;
- audio block, ring-buffer, DSP, and libebur128 primitives, plus exact originals-only timeline audio
  ranges, offsets, rate/reverse, overlap mix, clip/track gain/pan/fades, mute/solo, ordered
  EQ/compressor/dialogue-denoise/limiter processing, block-partition state continuity,
  normalization analysis, cancellation, and repeated request determinism; realtime
  prefill/callback/latency-compensated clock, submitted-position and
  uncertainty diagnostics, pause/resume, seek epoch invalidation, underrun zero-fill/diagnostics,
  end-of-stream demand, manual-device behavior, and the bounded asynchronous control facade's
  versioned requested/effective-state publication. Realtime EBU-R128 tests cover bounded-queue
  overload, reset generations, and shutdown under normal, ASan/UBSan, and TSan. Sample-range track
  meters prove that decode-ahead does not outrun the audio master. Desktop tests also require
  start/seek/pause Qt signal calls to return within 250 ms, selected/system-default startup,
  selected/default loss-return recovery, delayed stop, canceled recovery, stale normalization
  generations, and an opt-in 48 kHz physical-device smoke check with zero xruns. Accelerated one-hour
  zero-xrun and two-hour drift simulations are available through `VE_RUN_LONG_TESTS=1`; they do not
  satisfy the physical 10 ms / one-hour / two-hour gates (see lab protocol below).
- Protobuf framing/protocol compatibility and cancellation registry, plus worker probe/proxy and
  typed transcription-v2 request/range validation, monotonic events, streaming model byte ceilings,
  digest cancellation and atomic-replacement failures, exact FFmpeg source-window seek and
  16/44.1/48-kHz-to-mono-16-kHz trim boundaries, malformed/oversized backend-word rejection,
  unavailable-backend behavior, and a real framed worker-host transcription integration fixture;
- save/checkpoint faults (missing parent, parent-is-a-file, read-only overwrite leaves a good
  `.veproj` unchanged), cache `put_file` over-budget Full without touching originals, GPU
  `DeviceLost` keeping a CPU frame and the edit revision, controller preview without mutating
  revision, and worker-host SIGKILL during `JOB_KIND_PROXY` without a complete `.vepts` commit;
- Qt window/workspace/actions/accessibility basics, timeline interactions, and an end-to-end
  application import/edit/save/reopen/caption/export slice. Desktop coverage includes title,
  transition, speed, effect/keyframe/curve authoring, current-value mixer DSP controls,
  revision/target-bound normalization, lightweight encoder capabilities, hardware-to-software
  VP9 fallback progress, and creator Deliver options. Offscreen tests require non-empty accessible
  names on interactive controls, keep professional transport/workspace shortcuts bound, and walk a
  labeled Import → mixer/captions → Deliver beginner path without a full encode. Timeline coverage includes
  replace/toggle/range multi-selection, rich tool edge/body constraints, preview/single
  commit/Escape, canonical resolver and Shift bypass, exact frame-count nudging, marker/gap
  interaction, non-destructive context menus, track commands, linked split/delete, targeted
  insertion, atomic controller batches, caption style controls, model/review states, and proposal
  defaults. Live model inference and physical Vulkan transcription remain matrix
  tests rather than ordinary local tests. Worker-host SIGKILL during proxy and stub-exit death
  during desktop proxy generation are ordinary local tests.

Always use `ctest -N` for the count in the current build; the number changes as beta work lands.

## Physical-device A/V lab protocol

Before claiming Linux public-beta audio sync, run on a real output device with per-device calibration
saved from the Audio Mixer **Calibrate** control (QSettings key
`audio/calibratedLatencyFrames/<deviceId>`, or `audio/calibratedLatencyFrames/__system_default__`
for System default):

1. One hour of continuous 1× playback with zero audio xruns on the target Linux device.
2. Two hours of 1× playback with A/V drift versus program video remaining below 10 ms.

These runs require calibrated latency and real hardware. Accelerated fake-device tests and CI jobs do
not satisfy them. Residual clock uncertainty after calibration is expected and must not be reported
as zero.

## Corpus status

`tests/fixtures/corpus/manifest.json` remains a committed scaffold with one verifier fixture.
`python3 tools/quality/verify_corpus.py` still checks that scaffold. `--generated` and `--release`
validate `tests/fixtures/corpus/generated/` after `python3 tools/quality/generate_corpus.py` writes
at least 200 tiny MPL-2.0 lavfi fixtures covering every required category (VFR, B-frames, unusual
starting PTS, rotation, interlacing, 8/10-bit, HDR input, alpha, image sequences, corrupt inputs,
unusual channel layouts, SAR/field order, and damaged timestamps). Generated binaries are
gitignored; hashes are recorded in the same generation run. Third-party fixtures still require
documented redistribution rights. The generator is not a hardware decode-lab sign-off.

## CI intent and current limits

Repository workflows provide Linux CPU/core and sanitizer foundations plus a weekly
`linux-capability-matrix` probe (ffmpeg encoder/decoder highlights, Vulkan ICDs, skip-friendly
libplacebo). That workflow records skip/unavailable when a GitHub runner has no GPU; it is not a
hardware-lab sign-off. Windows nightly GPU/codec matrices are deferred with the Windows beta. Do
not infer Windows or GPU support solely from a Linux CPU workflow.

The dependency source/license script checks MPL presence, declared versions, packaging SPDX
markers, forbidden FFmpeg options, and release-source lock structure. An official run also examines
the actual loaded FFmpeg runtime. Source declaration and SBOM generation do not prove binary license
compliance.

## Required Linux-first public-beta acceptance gates

No Linux public beta should ship until all of the following are demonstrated on the **Linux**
supported matrix. Windows Intel/AMD/NVIDIA D3D11, signed MSI, and Windows Installer validation are
**not** first-beta gates.

- a first-time creator completes a captioned one-minute edit, dialogue adjustment, and creator
  export within 15 minutes without external documentation;
- a one-hour project with 1,000 clips, 16 video tracks, and 32 audio tracks remains responsive with
  bounded memory;
- one 4K30 proxy stream with basic color/transform plays without warm-state drops on baseline
  Linux hardware, proxy seek p95 is below 150 ms, and edit commit p95 is below 16 ms;
- one hour has zero audio xruns, and A/V error stays below 10 ms without two-hour drift;
- export frame and audio-sample counts are exact, cancellation never corrupts a destination, and
  recovery loses no completed edit command;
- supported Linux Intel/AMD/NVIDIA combinations pass Vulkan, Wayland, and X11 checks;
- every migration, save phase, worker death, GPU loss, disk-full case, missing/unplugged medium, and
  corrupt recovery case passes fault injection;
- accessibility review, beginner study, security review, dependency notices/source offers, and
  codec/patent review are complete.

The current application does not meet these gates because physical-device xrun/drift endurance on
the supported matrix, native event-driven hot-plug validation, non-1× realtime audio, approved
H.264/AAC export, physical multilingual/Vulkan transcription, worker fault injection, corpus, and
production Flatpak paths are incomplete. An engineering per-device latency calibration path now
exists in QSettings and the Audio Mixer; the 10 ms / one-hour / two-hour lab protocol above still
must pass on real hardware before beta. See the
[feature-status matrix](../beta-feature-status.md).

## Packaging gates

The Flatpak manifest is the first-beta packaging skeleton. The WiX MSI is deferred until after the
Linux-first public beta.

Before Flatpak distribution, replace the local directory source with immutable checksummed sources,
resolve every release-source lock, select an owned application ID/homepage, bundle the approved ABI
set, and pass `flatpak-builder-lint`, AppStream, dependency, codec, and Linux GPU tests.

Windows MSI work (DLL harvest, product identity, Authenticode signing, clean-machine
install/upgrade/uninstall) is out of first-beta scope. The current MSI skeleton remains unsigned
and the install rule only guarantees the application executable.

AppImage remains best-effort and is not a public-beta release gate.

## Release evidence

A release candidate should archive the commit and dependency lock, compiler/configure output,
actual runtime dependency audit, SPDX SBOM, full test and corpus results, performance/endurance
measurements, GPU/codec matrix, accessibility and usability findings, security review, package
validation, signatures, and legal approvals. A green unit-test summary alone is insufficient.

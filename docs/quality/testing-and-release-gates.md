<!-- SPDX-License-Identifier: MPL-2.0 -->

# Testing, quality, and release gates

Passing the current unit and integration suite means the engineering slice is internally
consistent; it does not satisfy the public-beta acceptance gates by itself.

## Local checks

For a full configured development tree:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
tools/quality/format_changed_cpp.sh --check
tools/quality/dependency_license_gate.sh --source-only
python3 tools/quality/verify_corpus.py
cmake --build build/dev --target sbom
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
  clip properties, audio-track mute/solo, canonical titles, and transition invariants;
- deterministic schema-v2 snapshot serialization, strict validation, schema-v1 backward reads,
  v1-field smuggling rejection (including additive track flags), old-v2 visible/targeted defaults,
  new track-state/title/transition round trips, and unknown-effect preservation;
- SQLite schema, revision conflicts, checkpoint cycles, transactional v1→v2 migration with validated
  pre-migration backup, failed-migration rollback, v1/v2 recovery discovery, recovery status, catalog
  sorting, corrupt candidates, and candidate bounds;
- FFmpeg probing, random/exact CPU frame requests, cancellation epochs, asset registry behavior,
  CPU transform/crop/opacity/blend and invisible-track behavior, deterministic title glyphs, cross-dissolve and
  dip-to-black transition boundaries, source-handle mapping, provider-failure propagation, request
  epochs and cache keys, deterministic video plus PCM export, exact decode-back frame/sample counts,
  and cancellation safety;
- pure GPU backend selection and unavailable-stub diagnostics, plus real Vulkan
  create/upload/normal-composite/download parity, per-clip GPU transform/composition behavior, and
  device-loss state when the exact dependency and a permitted device are available. Title and
  transition frames return typed `GpuUnsupportedTimeline` results, and desktop tests verify CPU
  fallback without permanently disabling the GPU; an opt-in desktop smoke check asserts that the
  per-clip GPU preview becomes active;
- proxy profile resolution, PTS-map validation/round-trip, transcode behavior, cancellation, and
  destination safety;
- subtitle validation/round-trip/reflow/search and caption application workflows;
- audio block, ring-buffer, DSP, and loudness primitives, plus exact originals-only timeline audio
  ranges, offsets, rate/reverse, overlap mix, gain/pan/fades, mute/solo, cancellation, and repeated
  request determinism; realtime prefill/callback/latency-compensated clock, submitted-position and
  uncertainty diagnostics, pause/resume, seek epoch invalidation, underrun zero-fill/diagnostics,
  end-of-stream demand, manual-device behavior, and the bounded asynchronous control facade's
  versioned requested/effective-state publication. Desktop tests also require start/seek/pause Qt
  signal calls to return within 250 ms, and an opt-in 48 kHz physical-device smoke checks zero xruns;
- Protobuf framing/protocol compatibility and cancellation registry, plus worker probe/proxy request
  validation, preset mapping, monotonic event sequences, and terminal errors;
- Qt window/workspace/actions/accessibility basics, timeline interactions, and an end-to-end
  application import/edit/save/reopen/caption/export slice. Timeline coverage includes
  replace/toggle/range multi-selection, rich tool edge/body constraints, preview/single
  commit/Escape, canonical resolver and Shift bypass, exact frame-count nudging, marker/gap
  interaction, non-destructive context menus, track commands, linked split/delete, targeted
  insertion, and atomic controller batches.

Always use `ctest -N` for the count in the current build; the number changes as beta work lands.

## Corpus status

`tests/fixtures/corpus/manifest.json` is a scaffold with one small verifier fixture, not the required
media corpus. Normal verification checks every present entry's safe path, length, and SHA-256 and
reports missing coverage. `--release` additionally requires `status: complete`, at least 200 unique
licensed files, and all required categories.

The final corpus must cover VFR, B-frames, unusual starting PTS, rotation, interlacing, 8/10-bit,
HDR input, alpha, image sequences, corrupt inputs, unusual channel layouts, SAR/field order, and
damaged timestamps. Generated fixtures require deterministic recipes; third-party fixtures require
documented redistribution rights.

## CI intent and current limits

Repository workflows provide Linux CPU/core and sanitizer foundations. Public beta additionally
requires nightly Windows/Linux codec and GPU matrices and weekly playback, memory-bound, recovery,
and packaging endurance runs. Do not infer Windows or GPU support solely from a Linux CPU workflow.

The dependency source/license script checks MPL presence, declared versions, packaging SPDX
markers, forbidden FFmpeg options, and release-source lock structure. An official run also examines
the actual loaded FFmpeg runtime. Source declaration and SBOM generation do not prove binary license
compliance.

## Required public-beta acceptance gates

No public beta should ship until all of the following are demonstrated on the supported matrix:

- a first-time creator completes a captioned one-minute edit, dialogue adjustment, and creator
  export within 15 minutes without external documentation;
- a one-hour project with 1,000 clips, 16 video tracks, and 32 audio tracks remains responsive with
  bounded memory;
- one 4K30 proxy stream with basic color/transform plays without warm-state drops on baseline
  hardware, proxy seek p95 is below 150 ms, and edit commit p95 is below 16 ms;
- one hour has zero audio xruns, and A/V error stays below 10 ms without two-hour drift;
- export frame and audio-sample counts are exact, cancellation never corrupts a destination, and
  recovery loses no completed edit command;
- supported Windows Intel/AMD/NVIDIA and Linux Intel/AMD/NVIDIA combinations pass D3D11, Vulkan,
  Wayland, and X11 checks;
- every migration, save phase, worker death, GPU loss, disk-full case, missing/unplugged medium, and
  corrupt recovery case passes fault injection;
- accessibility review, beginner study, security review, dependency notices/source offers, and
  codec/patent review are complete.

The current application does not meet these gates because physical-device xrun/drift and latency
calibration, non-1× realtime audio, native GPU presentation/effect-color parity, creator delivery,
worker, corpus, and production-packaging paths are incomplete. See the
[feature-status matrix](../beta-feature-status.md).

## Packaging gates

The Flatpak manifest and WiX MSI are development skeletons.

Before Flatpak distribution, replace the local directory source with immutable checksummed sources,
resolve every release-source lock, select an owned application ID/homepage, bundle the approved ABI
set, and pass `flatpak-builder-lint`, AppStream, dependency, codec, and GPU tests.

Before MSI distribution, install every required DLL and notice, finalize product/manufacturer and
upgrade identity, add file associations as approved, sign and timestamp, and pass clean-machine
install/upgrade/uninstall plus Windows Installer validation and malware scanning. The current MSI is
unsigned and the install rule only guarantees the application executable.

AppImage remains best-effort and is not a public-beta release gate.

## Release evidence

A release candidate should archive the commit and dependency lock, compiler/configure output,
actual runtime dependency audit, SPDX SBOM, full test and corpus results, performance/endurance
measurements, GPU/codec matrix, accessibility and usability findings, security review, package
validation, signatures, and legal approvals. A green unit-test summary alone is insufficient.

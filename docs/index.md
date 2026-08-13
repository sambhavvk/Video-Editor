<!-- SPDX-License-Identifier: MPL-2.0 -->

# Video Editor documentation

Video Editor is an offline-first Windows and Linux editor under active development. The current
repository is an engineering vertical slice, **not a public beta**. It already proves
exact edits, transactional projects, software preview, local proxies, captions, safe reference
masters, and FOSS VP9/Opus creator delivery. A capability-gated libplacebo D3D11/Vulkan engine composes
supported active clips on the GPU before the CPU fallback; the current viewer downloads an offscreen
image rather than using native-swapchain presentation. Realtime 48 kHz audio
device/master-clock playback is connected for forward 1× transport in builds with the pinned
miniaudio adapter; it reports submitted versus latency-compensated positions and uncertainty.
The CPU reference graph renders titles, transitions, typed effect curves, color/crop/blur nodes,
and the ordered track-audio DSP chain. FOSS creator delivery can use QSV/VAAPI VP9 with complete
libvpx retry. Track/master meters, worker-owned EBU-R128 loudness, editable normalization, and
polled audio-device recovery are connected. H.264/AAC approval, the physical audio/GPU matrix, and
local transcription are not complete.

The desktop timeline now exposes atomic multi-selection edits, linked A/V split/delete,
normal/ripple/overwrite trims, roll/slip/slide tools, track management, marker/gap interaction, and
controller-backed exact frame/marker snapping. These complete the professional timeline interaction
slice but do not imply that the other beta areas in the status matrix are finished.

Choose a starting point:

- [User guide](user-guide.md) — workflows that can be used in the current desktop build, current
  shortcuts, and visible limitations.
- [Beta feature status](beta-feature-status.md) — implemented, partial, missing, and intentionally
  deferred capabilities.
- [Build and dependencies](developer/build-and-dependencies.md) — Linux and Windows `clang-cl`
  development setup and dependency rules.
- [Architecture overview](architecture/overview.md) — modules, ownership boundaries, and runtime
  data flow.
- [Exact timeline semantics](reference/timeline-semantics.md) — rational time, revisions,
  overlap policy, linked edits, and precision commands.
- [Project format and recovery](reference/project-format-and-recovery.md) — `.veproj`, the working
  database, checkpoints, journaling, and startup recovery.
- [Media, proxies, and cache](reference/media-proxies-and-cache.md) — fingerprints, proxy profiles,
  PTS sidecars, authoritative originals, and present cache limitations.
- [Testing and release gates](quality/testing-and-release-gates.md) — local checks, fixtures,
  packaging state, and the remaining public-beta gates.

## Source API references

- [Desktop UI](../src/desktop_ui/doc/index.md)
- [Edit model](../src/edit_model/doc/index.md)
- [Project codec](../src/project_codec/doc/project_codec.md)
- [Project store](../src/project_store/doc/ProjectStore.md)
- [Render engine](../src/render_engine/doc/index.md)

## Architecture decisions

Accepted decisions live in [`architecture/`](architecture/):

1. [Native modular editor foundation](architecture/0001-foundation.md)
2. [SQLite working database and checkpoint files](architecture/0002-project-persistence.md)
3. [MPL application and dynamic media dependencies](architecture/0003-licensing.md)
4. [CPU preview, captions, and reference export](architecture/0004-cpu-preview-captions-export.md)
5. [Precision edits and linked-clip semantics](architecture/0005-precision-edit-commands.md)
6. [Interactive timeline gesture boundary](architecture/0006-interactive-timeline-gestures.md)
7. [Versioned proxy PTS map](architecture/0007-proxy-pts-map.md)
8. [Read-only startup recovery catalog](architecture/0008-recovery-catalog.md)
9. [Originals-only 48 kHz timeline audio and mux](architecture/0009-timeline-audio-render-and-mux.md)
10. [Realtime audio master-clock playback](architecture/0010-realtime-audio-master-clock.md)
11. [Capability-gated libplacebo GPU presentation](architecture/0011-gpu-presentation.md)
12. [Rebuildable media cache (thumbnails, waveforms, metadata)](architecture/0012-media-cache.md)
13. [Schema v2 title and transition contracts](architecture/0013-schema-v2-titles-transitions.md)
14. [Professional timeline interaction boundary](architecture/0014-professional-timeline-interaction.md)
15. [Clip-local effect curves and CPU reference effects](architecture/0015-effect-parameter-authoring.md)
16. [FOSS creator delivery with VP9 and Opus](architecture/0016-foss-creator-delivery.md)
17. [Professional track audio, DSP, meters, and normalization](architecture/0017-professional-audio-workflow.md)

Source code and documentation in this repository use MPL-2.0. Third-party libraries and media
fixtures retain their own licenses; see the repository's `THIRD_PARTY.md` and fixture manifests.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# B01: Baseline workflow verification

Recorded 12 September 2026 on commit `edb7399` plus the source/program In/Out wiring fix in this
change. This is observed evidence for the current desktop vertical slice, not a public-beta sign-off.

## How this was verified

Workflows were exercised through the existing offscreen Qt controller/window tests and GTest
service tests after `cmake --preset dev` / `cmake --build --preset dev`. The desktop GUI was not
launched as an interactive editing session. Physical decode-lab, xrun/drift, and Vulkan presentation
claims remain unverified.

`tools/quality/dependency_license_gate.sh --source-only` passed. Official Flatpak source locks remain
development blockers (`video-editor-source`, `ffmpeg-lgpl-9.0.1`, `distribution-dependency-lock`).

## Machine and versions

| Item | Observed |
| --- | --- |
| Host | Linux x86_64, kernel 7.2.3-1-cachyos |
| Compiler | GCC 16.2.1 |
| Qt | 6.11.2 (test library and `qt6-base 6.11.2-3.1`) |
| FFmpeg | n9.0.1 (`ffmpeg 2:9.0.1-4.1`) |
| libplacebo | 7.360.1 |
| SQLite | 3.53.4 |
| Protobuf (runtime) | 36.1.0 (source contract in `cmake/DependencyVersions.cmake` is 35.1) |
| GPU headers | `vulkan-headers` 1.4.357.0 installed for this rebuild |

## Required workflows

| Workflow | Status | Evidence | Notes |
| --- | --- | --- | --- |
| Import | Supported | `EditorControllerTest::importsInsertsAndRoundTripsUndo`, `insertAssetInsertsImportedMediaWithoutSourceMonitor`, `AgentHostTest.ImportInsertAndPreviewProducesPng` | File-dialog and drag-drop UI not covered. |
| Source trim (I/O + insert) | Supported after B01 fix | `loadsSourceMonitorAndRippleInsertsMarkedRange` (was failing: start=0 then 9600), `markInOutActionsTargetFocusedViewer`, `inOutActionMarksProgramWhenSourceIsHidden` | See [In/Out fix](#inout-fix). **L** on the source viewer still ripple-inserts instead of shuttling (U01). |
| Timeline edit | Supported | `professionalTimelineInteractionsUseOneAtomicHistoryStep`, `razorAddEditsSplitsUnlockedClips`, `timelineRazorHandAndZoomTools`, `TimelineEditorTest` split/ripple/remove | Controller clip-delete after import is not an integration test; model delete is covered. |
| Save / reopen | Supported | `importsInsertsAndRoundTripsUndo` (`.veproj` checkpoint + reopen clip/asset counts), `ProjectStoreTest` checkpoint/journal | No test continues trim/export after reopen. Agent `open_project`/`save_project` untested. |
| Captions | Supported | `importsSearchesAndExportsCaptions` (SRT import, search, table edit, VTT export, add/undo), caption parser/serialization tests | Captions surviving save/reopen is not an end-to-end controller test. |
| Export | Supported (service) / partial (desktop creator) | Controller FFV1 `master.ffv1` in `importsInsertsAndRoundTripsUndo`; `ExportService` exact Rec.709 FFV1, VP9/Opus WebM (VAAPI unavailable → `libvpx-vp9` 1.17.0), podcast Opus, embedded WebVTT | Creator WebM is not encoded through the full Deliver queue in a desktop test. |

## Feature-matrix comparison

Compared with [beta-feature-status.md](../beta-feature-status.md):

- **Implemented rows that this run supports:** desktop shell (offscreen), exact timeline/revisions, insert/move/trim/split (tested subset), interactive tools (razor/hand/zoom signals), media import/probe (lavfi/WAV fixtures), CPU preview (source + program frames), SRT/WebVTT captions, reference FFV1 export, project checkpoints.
- **Partial, matching the matrix:** GPU preview (offscreen tests warn `Failed to initialize Vulkan`; no native swapchain evidence), creator delivery (service-level VP9/Opus pass; desktop queue encode unverified), captions burn-in (service tests, not this B01 pass), schema/history breadth, accessibility/human study.
- **Missing, matching the matrix:** H.264/AAC legal distribution.
- **Unverified here:** physical audio xrun/drift lab, physical relink/unplug, multilingual/Vulkan transcription, Flatpak store lint, large-track performance, Windows GPU/MSI.

Discovered during B01 and fixed in this change: source I/O did not apply the marked range. The matrix already described I/O as working; the tests now match that claim.

## In/Out fix

`loadsSourceMonitorAndRippleInsertsMarkedRange` failed twice:

1. `sourceMarkInRequested` was connected to `markProgramIn`. The test (and source-viewer keys) call `EditorWindow::markSourceIn()` without relying on source focus, so `source_mark_in_` stayed empty and inserts used `0…duration`.
2. After wiring source marks, `markedSourceRange()` tagged 48 kHz UI ticks with the sequence `timelineTime()` scale (`lcm(48000, 30000) = 240000` on the default 29.97 sequence), so 1 s became 0.2 s (`9600` at 48 kHz).

The I/O **shortcut** still has to mark the focused viewer so program In/Out (loop and export range) remain reachable. `EditorWindow` now dispatches `I`/`O` like `J`/`K`: source focus → source marks; otherwise → program marks. `window.markSourceIn()` (source-viewer keys and tests) always sets source marks. `markedSourceRange()` uses `kUiTimescale`.

## Later chunks

Each later roadmap chunk is a **new feature**, an **extension** of something already present, or **verification-only**.

| ID | Class | Why |
| --- | --- | --- |
| B02 | Extension | `THIRD_PARTY.md`, SPDX SBOM, and the license gate exist; B02 must pin exact versions/eligibility and block unresolved items (whisper model `NOASSERTION`, Qt 6.11.1 vs 6.11.2 doc drift, protobuf 35.1 vs 36.1). |
| B03 | Extension | Synthetic lavfi corpus generator exists; B03 adds repeatable YouTube/interview/film scenarios and measurement records. |
| U01 | Extension | Remappable insert/overwrite commands exist; **L** with source focus still inserts instead of shuttle. |
| U02 | Extension | Source/program viewers, titles, and timecode exist; focus/identity must be unmistakable. |
| U03 | Extension | Four workspaces persist; compact 980×680 / maximize-panel / reset are not verified. |
| U04 | Extension | Precision toolbar and tool shortcuts exist. |
| U05 | Extension | Dark theme and accessible names exist; scaling/contrast/density remain. |
| U06 | New feature | Named Creator/Film (and Text/Audio) presets are not the current Import/Edit/Audio/Deliver set. |
| E01 | Extension | Selection, linked selection, locks, and targeting exist; destination feedback is incomplete. |
| E02 | Extension | Transient trim previews exist; frame deltas and two-up comparison are incomplete. |
| E03 | New feature | Linked A/V exists; visible sync offset and resync action do not. |
| E04 | Extension | Track height via header wheel exists; named visibility presets and searchable track nav do not. |
| R01 | New feature | Preview profiles exist internally; Full/Half/Quarter UI does not. |
| R02 | Extension | Background jobs and status exist; pause/resume admission during interaction does not. |
| R03 | Extension | Checkpoints and recovery catalog exist; named restore-point UI does not. |
| R04 | New feature | Relink, cache, and job errors exist separately; no aggregated health panel. |
| R05 | Verification-only | Lab protocols and scripts exist; physical open-stack measurements are not done. |
| D01 | Extension | Color controls exist; explicit source/sequence interpretation UI is incomplete. |
| D02 | Verification-only | CPU/GPU/export paths exist; documented preview/export tolerances are not proven. |
| D03 | Extension | Deliver panel and queue exist; one coherent capability summary is incomplete. |
| D04 | New feature | Queue persists; saved delivery recipes do not. |
| C01 | Extension | Transcript search/navigation exist; speaker labels and spelling-vs-cut separation need work. |
| C02 | New feature | Timed words exist; multi-clip paper-edit assembly does not. |
| C03 | Extension | Silence/filler proposals exist; breathing room, protected ranges, and audition need work. |
| C04 | New feature | Titles/captions/effects exist; reusable channel kits do not. |
| C05 | New feature | Volume envelopes exist; dialogue-driven ducking does not. |
| C06 | New feature | Nested/duplicate sequences exist; labeled independent aspect-ratio copies do not. |
| M01–M05 | New feature | Multicam is deferred in the status matrix. |
| F01 | Extension | Asset metadata/tags exist; production scene/take fields do not. |
| F02 | New feature | Source ranges exist on clips; subclip objects do not. |
| F03 | New feature | Import probes channels; stereo monitoring path does not preserve production-audio identities. |
| F04 | New feature | Timeline audio export exists; handle-aware stem export does not. |
| F05 | Extension | OTIO JSON import/export exists; handoff report/package does not. |
| F06 | Verification-only | Depends on F01–F05 plus a real 60–120 minute project. |
| V01 | New feature | Sequence duplication exists; named version comparison does not. |
| V02 | New feature | Markers exist; durable review notes do not. |
| V03 | New feature | Relink and cache exist; travel-kit packaging does not. |
| X01–X08 | New feature (discovery) | Scope-only; not implementation. |

## Follow-ups recorded, not fixed

- **U01:** Source-viewer **L** still calls `rippleInsertFromSource` (`ProgramViewer` source-edit keys and the Forward action). Keep J/K/L as transport; give insert/overwrite explicit remappable commands.
- **B02:** Align `THIRD_PARTY.md` / developer docs (Qt 6.11.1) with `VIDEO_EDITOR_QT_VERSION` 6.11.2; record protobuf 36.1 local ABI vs 35.1 contract; resolve whisper.cpp model terms (`NOASSERTION`).
- Captions and creator-delivery save/reopen/queue encodes remain integration gaps, not blockers for B01.
- Interactive GUI session, Vulkan presentation, and physical labs remain unverified.

## Next eligible chunk

**B02 — Establish the FOSS component register.**

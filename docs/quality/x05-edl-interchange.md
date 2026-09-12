# X05 — CMX3600 EDL interchange (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X05). Discovery only; no implementation claimed.

## Options considered

| Option | Why not first |
| --- | --- |
| Script / take comparison | No script model |
| Continuity notebook | New product surface |
| Verified dailies ingest | Ingest + checksum pipeline large |
| EDL / AAF / XML exchange | [F05](f05-otio-handoff.md) + [OTIO ADR](../architecture/0021-otio-interchange.md) exist; EDL is simplest FOSS text format | **Selected** |

AAF and proprietary XML SDKs remain **not selected** per [FOSS component register](foss-component-register.md).

## Chosen workflow: CMX3600 EDL import and export (first slice)

### End-to-end first slice

1. **Export:** From active sequence, **File → Export EDL (CMX3600)…** writes a `.edl` with:
   - `TITLE`, `FCM: NON-DROP FRAME` (or drop-frame when sequence rate is NTSC drop).
   - One `EVENT` per video clip on targeted tracks (v1: single video track + optional audio `AX` lines when clip has audio).
   - Source reel name = asset basename; source In/Out from clip handles; record In/Out from timeline.
2. **Import:** **File → Import EDL…** creates a **new sequence** (non-destructive) with offline clips when media paths in comments are missing; relink via existing asset workflow.
3. **Handoff report:** Plain-text sidecar (like F05) listing unsupported events (transitions, effects, nested sequences, multicam).

### FOSS requirements

- First-party CMX3600 parser/serializer in `video_editor_interchange` (MPL-2.0), same module boundary as OTIO JSON.
- No OpenTimelineIO C++ library, no Avid AAF SDK, no closed parsers.
- Timecode: reuse rational time + NTSC helpers from edit model.

### Receiving workflow

| Receiver | Expectation |
| --- | --- |
| VideoEditor | Import creates sequence + media bin placeholders |
| External NLE (e.g. DaVinci, kdenlive) | Export EDL + F05 OTIO package for richer handoff; EDL for picture-cut list only |
| Travel kit ([V03](v03-travel-kit.md)) | Optional future: bundle `.edl` beside `timeline.otio` |

### Explicit unsupported list (v1)

- Transitions, titles, speed effects, opacity, color grades, nested sequences, multicam groups.
- Split edits, dissolve comments, multiple audio channels beyond one `AX` per event.
- AAF, FCPXML, Premiere XML, encrypted media.

## Non-goals (leave for later)

- Complete dailies suite, script sync, continuity notebook.
- Multiple interchange formats in one implementation pass.
- Round-trip fidelity with every commercial NLE feature.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X05a | CMX3600 lexer + event model + unit tests (golden files) |
| X05b | Export from flat sequence (no nests) |
| X05c | Import → new sequence + handoff report |

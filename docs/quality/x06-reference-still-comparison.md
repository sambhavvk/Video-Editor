# X06 — Reference-still comparison (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X06). Discovery only; no implementation claimed.

## Options considered

| Option | Why not first |
| --- | --- |
| Reference-still comparison | No viewer A/B against still | **Selected** — QC without tracking ML |
| Secondary correction / masks | Partial color stack exists | Masks need rotoscope model |
| Tracking / stabilization | Deferred in beta matrix | FOSS trackers (OpenCV) heavy + patent review |
| Tracked reframing | C06 residual | Depends on tracking |

## Chosen workflow: program vs reference still overlay

### Bounded sample workflow

1. User imports a `.png` / `.jpg` reference (color card, brand frame, previous cut).
2. **View → Reference still…** pins the image to the program viewer as a **comparison layer** (not a timeline clip).
3. Modes: **Side by side**, **Overlay** (opacity slider), **Difference** (absolute RGB delta, Rec.709 space).
4. Reference does not export, burn in, or appear in OTIO. Grab Frame already writes
   `currentDisplayImage()` (preview display); with a reference pinned it includes the overlay when
   visible. That is the v1 screenshot path — not a full-resolution still export.

### FOSS implementation

| Piece | Path |
| --- | --- |
| Still decode | Existing FFmpeg still import / `QImage` load |
| Composite | CPU `CpuRenderer` blend atop program frame; optional libplacebo overlay when GPU preview active |
| Persistence | Session-only QSettings path list; **not** in `.veproj` v1 (avoid schema churn) |
| Manual fallback | If GPU overlay fails, CPU difference mode always available |

### Quality limits

- Comparison uses **preview resolution** (respects R01 quality / future Auto); export oracle unchanged.
- Difference mode is diagnostic, not legal grade; no HDR scene-referred math.
- No automatic alignment — user scales/positions reference with on-viewer handles (transform stored in session).

### Deterministic project state

- Reference still is **non-authoritative** UI state; undo stack unaffected.
- Reload project clears reference unless user re-pins (document in UI).

### Preview / export agreement

- Export ignores reference layer (test: export hash unchanged with reference pinned).
- Grab-frame includes reference when overlay visible (matches today's display-image grab).

## Non-goals (leave for later)

- Timeline-based reference clip, tracked match move, stabilization, secondary masks.
- Simultaneous tracking + compositing + HDR development.
- ML-based auto-align.
- Full-resolution grab-frame independent of preview scale.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X06a | `ReferenceStillController` session state + file picker |
| X06b | Program viewer overlay modes + opacity |
| X06c | Tests: export unchanged; grab-frame optional include |

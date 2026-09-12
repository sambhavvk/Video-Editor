# X01 — Adaptive preview quality (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X01). Discovery only; no implementation claimed.

## R05 bottleneck evidence

[R05 playback validation](r05-playback-validation.md) (2026-09-12, offscreen CI) did **not** run the
physical decode-lab sync/drift matrix. Automated suites passed except one pre-existing render golden
(`CpuRenderer.CrossDissolveUsesHandleExtrapolationAndHalfOpenRange`). Fault-injection ctest was
skipped (missing `libprotobuf.so.36` on the host).

Indirect evidence still points to one bottleneck for this scope:

| Signal | Source | Implication |
| --- | --- | --- |
| Manual Full/Half/Quarter only; no Auto | [R01](r01-preview-quality.md) residual | Editors must guess quality; sustained playback decode cost is unmanaged |
| In-memory LRU `RenderCache` (revision/time/dimensions/graph signature) | `beta-feature-status.md`, `render_cache.h` | Reuses **decoded frames** but does not reduce decode cost on cache miss |
| Disk `CacheStore` + proxies | ADR 0012, `media-proxies-and-cache.md` | Off-line transcode path; not a per-sequence in/out preview bake |
| Audio adaptive decode-ahead buffer | `beta-feature-status.md` mixer row | Audio already adapts to xruns; video preview does not |

**Chosen bottleneck:** sustained **program-preview CPU decode + composite cost** on cache miss during
playback and scrub (not export, not proxy generation). Physical lab confirmation remains a gate before
shipping Auto quality.

**Rejected for this scope:** explicit in/out **disk render caching** — overlaps proxy + rebuildable
`CacheStore` semantics, needs heavy invalidation on every edit, and does not address live scrub as
directly as adaptive quality. Defer as a separate chunk if lab data shows decode is already fast enough
at Quarter but range replay is still stuttery.

## Supported path: adaptive preview quality

Extend R01's manual presets with an **Auto** mode that temporarily downgrades effective preview
resolution during transport stress and restores the user's chosen ceiling when idle.

### UI states

| State | Status bar / viewer | User control |
| --- | --- | --- |
| Manual Full/Half/Quarter | Unchanged from R01 | Combo selection; persists in `preview/qualityScale` |
| Auto (new) | Title shows `Auto → Half` (or Quarter) when downgraded; `Auto (Full)` when at ceiling | Fifth combo entry; ceiling stored as today (last manual preset before Auto, or explicit sub-combo later) |
| Stress | Brief tooltip or HUD line: "Preview quality reduced for playback" | None required; optional "Lock quality" override defers downgrade for one session |
| GPU path | Same indicators; downgrade applies to decode dimensions before GPU upload | GPU latch-to-CPU rules unchanged |

Auto must **never** change export, proxy jobs, or disk cache contents.

### Cancellation and invalidation

- **Revision change:** clear in-memory `RenderCache` (existing behavior); reset stress counter.
- **Manual preset change while playing:** apply immediately; cancel pending downgrade timer.
- **Stop transport:** restore effective quality to ceiling within one frame presentation (no stuck Quarter).
- **Scrub end (debounce):** same as R02 admission resume (~existing scrub debounce); quality may step up one level at a time to avoid flicker.
- **Proxy adoption / qualityScale persist:** no automatic proxy creation; Auto only scales decode/resolution.
- **Session end:** persist `preview/qualityMode` = `manual` \| `auto` and ceiling preset in QSettings.

### Measurable acceptance criteria

1. With a reference 4K long-GOP clip on CPU preview at Full ceiling, enabling Auto keeps median
   program frame time below a documented threshold (e.g. ≤ 33 ms for 30 fps) by stepping down to Half
   then Quarter within N consecutive slow frames.
2. Pausing playback restores Half/Full within 500 ms without user action.
3. Export and grab-frame still use full resolution and originals (regression test on existing export suite).
4. `EditorControllerTest` + one new test: Auto downgrades under injected slow-render hook and restores on stop.
5. Physical decode-lab run recorded before beta claims "adaptive preview" as Implemented.

## Non-goals (leave for later)

- Disk-backed in/out preview render cache and background bake jobs.
- Assuming Vulkan/GPU presentation removes the need for Auto on all hardware.
- Adaptive proxy generation or automatic Half proxy creation.
- Source monitor / trim two-up dimension scaling (R01 residual stays until a follow-up).
- Implementing Auto and disk range cache together.

## Follow-up chunks (implementation)

| ID | Work |
| --- | --- |
| X01a | `PreviewQualityMode` enum, QSettings, status-bar combo + program title strings |
| X01b | Transport stress detector (frame time EMA) and stepped downgrade/upgrade policy |
| X01c | Tests + update `beta-feature-status.md` after physical lab sign-off |

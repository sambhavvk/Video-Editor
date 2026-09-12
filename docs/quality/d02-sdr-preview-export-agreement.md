# D02 — SDR preview/export agreement

Verified on branch `beta-1.0-fix` (phase 5 D02), Linux offscreen.

## Supported path

- **Preview:** CPU/GPU Rec.709 SDR reference canvas; proxies permitted only when preview quality allows.
- **Export:** `export_service` always uses full-quality originals, `permit_proxy=false`, and
  `AVCOL_RANGE_MPEG` limited Rec.709 output.

## Automated evidence

| Test area | Result |
|-----------|--------|
| `video_editor_export_service_tests` (19) | Pass — export dimensions, originals, no proxy |
| `render_engine` preview profile tests | Pass — blur bypass/proxy gating isolated from export |
| `video_editor_render_engine_tests` | 59/60 pass; one CrossDissolve golden unrelated to color range |

## Tolerances

- Preview may use reduced resolution/effects; export does not inherit preview shortcuts (R01).
- CPU preview uses documented Rec.709 approximation; export uses deterministic limited-range packing.

## Unsupported / declared

- HDR tone mapping, display calibration, and hardware decode are out of scope.

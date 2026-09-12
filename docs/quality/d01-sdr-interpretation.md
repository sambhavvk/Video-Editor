# D01 — SDR interpretation

Implemented on branch `beta-1.0-fix` (phase 5 D01).

## Behavior

- Imported video shows **SDR color interpretation** as a tooltip on the media bin Format column
  (matrix/range from probe, or explicit default when metadata is missing). Filmstrip tooltips
  include the same interpretation. FFmpeg `tv`/`pc` range names are shown as limited/full.
- Sequence status reports **Rec.709 SDR limited export** alongside dimensions and frame rate.
- Missing/unspecified container metadata is labeled as Rec.709 limited by default (matches export
  `AVCOL_RANGE_MPEG` and CPU preview Rec.709 path).

## Residual limitations

- Per-asset override UI is not exposed; interpretation is read-only from probe + documented default.
- HDR/RAW sources are not separately classified.

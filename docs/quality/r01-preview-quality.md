# R01 — Manual preview quality

Implemented on branch `beta-1.0-fix` (phase 4 R01).

## Behavior

- Status-bar **Full / Half / Quarter** combo controls program and source preview resolution.
- Program viewer title reports quality, **proxy**, and **reduced effects** indicators; backend label
  (CPU/GPU) follows existing GPU init and fallback messaging.
- Choice persists in `preview/qualityScale` QSettings and survives restart.
- Export path is unchanged: full resolution, originals, no blur bypass.

## Tests

- `EditorControllerTest::previewQualityPersistsAndUpdatesProgramTitle`
- `EditorWindowTest::previewQualityComboEmitsPreset`

## Residual limitations

- No adaptive Auto quality; playback no longer auto-downgrades to Quarter on CPU transport.
- Indicators reflect preview policy, not per-clip proxy adoption at the current frame.
- Trim two-up and source monitor honor proxy permission but do not scale decode dimensions yet.

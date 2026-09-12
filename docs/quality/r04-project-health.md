# R04 — Project health panel

Implemented on branch `beta-1.0-fix` (phase 4 R04).

## Behavior

- **View > Project Health** dock aggregates offline/changed media, missing recommended proxies,
  full media cache, background job failures, and failed exports.
- **Repair selected** routes to relink, proxy generation, cache browser, export retry, or dismiss
  for logged failures.
- **Refresh** rebuilds the list from current project state (also updated on `refreshViews()`).

## Tests

- `EditorWindowTest::projectHealthPanelExposesRepairControls` (UI shell)

## Residual limitations

- Unavailable fonts/effects are not surfaced yet.
- Proxy mismatch when a stale proxy file exists but is not adopted remains implicit.
- Repair dismisses background failures instead of retrying the underlying job automatically.

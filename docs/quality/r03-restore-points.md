# R03 — Named restore points

Implemented on branch `beta-1.0-fix` (phase 4 R03).

## Behavior

- **File > Restore Points…** lists named `.veproj` checkpoints stored under
  `{recovery}/restore-points/` plus recommended recovery working databases.
- **Create restore point** writes a checkpoint and appends metadata to `manifest.json` (name, time,
  sequence, revision).
- **Restore** previews the choice in-dialog; unsaved work can be preserved by creating a safety
  restore point before loading the selected checkpoint or recovery session.

## Tests

- `EditorControllerTest::namedRestorePointsPersistManifest`

## Residual limitations

- Manifest does not prune missing checkpoint files automatically.
- No side-by-side revision diff; preview is descriptive only.
- Never-saved projects without a working database are out of scope.

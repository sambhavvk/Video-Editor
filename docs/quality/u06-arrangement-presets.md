# U06 — Creator, Film, Text, and Audio arrangement presets

Implemented in commit `83681c0` (adds
`ArrangementPreset`, View > Arrangements actions, and QSettings persistence under
`ui/arrangements/v1/<preset>/`).

## Presets

| Preset | Workspace | Source monitor | Primary panels |
| --- | --- | --- | --- |
| Creator | Edit | Hidden | Media, inspector, program |
| Film | Edit | Visible | Media, inspector, dual viewers |
| Text | Audio & Captions | Hidden | Captions, inspector |
| Audio | Audio & Captions | Hidden | Mixer, captions |

## Reset and persistence

- **View > Reset Arrangement Preset** restores the default layout for the active preset.
- Switching presets saves dock layout plus workspace and viewer visibility flags.
- Reopen restores the last preset and its saved arrangement snapshot.

## Tests

- `EditorWindowTest::arrangementPresetsComposeViewers`
- `EditorWindowTest::persistsWorkspaceAndProgressiveControls` (reopen path)

# C04 — Channel style kits

Implemented on branch `beta-1.0-fix` (phase 6 C04, commit `c9e0d39`).

## Behavior

- Built-in lower third, chapter card, and caption style kits in the captions panel; custom kits save to `creator/styleKits` QSettings JSON.
- Apply updates caption styles and inserts title clips for graphic presets; attribution label shows Noto Sans OFL notice.
- Unsupported font families show a fallback explanation before apply.

## Tests

- Manual: apply each built-in kit; save custom kit and verify reload after restart.

## Residual limitations

- Kits persist in window settings, not in the project snapshot.
- No marketplace or motion-graphics engine beyond title + caption styling.

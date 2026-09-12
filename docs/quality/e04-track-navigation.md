# E04 — Track navigation presets

Implemented on branch `beta-1.0-fix` (phase 3 E04).

## Behavior

- **Track navigation** panel above the marker list: searchable track list, visibility presets
  (All / Dialogue / Music / Effects / Video only), Restore snapshot, and Compact/Normal/Expanded
  height presets.
- Double-clicking a track scrolls it into view via `TimelineWidget::focusTrack` without seeking or
  changing clip selection.
- Visibility presets batch `SetTrackVisibilityCommand` edits (one undo step). Restore reapplies the
  pre-isolate snapshot in one undo step.

## Tests

- `EditorWindowTest::trackNavFiltersAndFocusesTracks`
- `EditorWindowTest::trackNavAppliesHeightPresets`
- `EditorControllerTest::trackVisibilityPresetIsolatesAndRestores`

## Residual limitations

- Preset matching is name-heuristic (dialogue/music/effects keywords), not persistent track groups.
- Height presets are session UI state only (not saved in `.veproj`). The height combo does not
  follow later header-wheel adjustments.
- Restore remembers one pre-isolate snapshot for the sequence that was isolated; switching
  sequences hides Restore until that sequence is active again. New/open project clears it.
- No overview/minimap strip.

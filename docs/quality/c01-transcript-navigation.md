# C01 — Transcript reading and navigation

Implemented on branch `beta-1.0-fix` (phase 6 C01, commit `904760a`).

## Behavior

- Captions panel adds a Speaker column (WebVTT identifier or `<v Name>` voice tags).
- Search highlights matched UTF-8 spans in the caption column; playhead sync highlights the active cue/word.
- Single-click cue selection seeks; word activation seeks word start.
- Spelling edits use `UpdateCaptionCommand` with `CaptionWordSource::UserEdited`; review cuts stay separate.

## Tests

- `EditorControllerTest::importsSearchesAndExportsCaptions`
- `EditorWindowTest::captionsPanelEmitsEditableCueActions`
- `EditorWindowTest::captionsPanelExposesTranscriptionAndWordNavigation`

## Residual limitations

- No automatic speaker diarization; labels come from import metadata or WebVTT voice tags only.
- Search highlights are per-cue filter rows, not a global next/previous hit navigator.

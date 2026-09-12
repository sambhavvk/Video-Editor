# E03 — Linked A/V sync offset and resync

Implemented on branch `beta-1.0-fix` (phase 3 E03).

## Behavior

- Linked groups compute audio-minus-video timeline offset in exact sequence frames.
- Clips with non-zero offset show an `A/V ±Nf` badge on the timeline.
- The Inspector **Linked A/V sync** group explains the offset and exposes **Resync linked clips**.
- Resync moves linked partners to the active clip’s timeline start with `include_linked=false` (one undo step).

## Tests

- `EditorWindowTest::inspectorShowsLinkedAvSyncControls`

## Residual limitations

- Offset is timeline-start based, not source-time or drift-aware.
- No dedicated status-bar summary; Inspector + clip badge only.

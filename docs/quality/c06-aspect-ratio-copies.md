# C06 — Explicit aspect-ratio copies

Implemented on branch `beta-1.0-fix` (phase 6 C06, commit `b9769b3`).

## Behavior

- Timeline menu **Create Aspect-Ratio Copies…** adds `{name} (Landscape 16:9)` at 1920×1080 and `{name} (Vertical 9:16)` at 1080×1920 in one undo step.
- Vertical copy raises caption safe margin and clamps vertical position; start markers label the independent relationship.
- Copies do not link edits; user message states no automatic sync.

## Tests

- Manual: create copies, edit one sequence, confirm the other is unchanged; export both.

## Residual limitations

- No tracked reframing or edit propagation between variants.
- Framing guides are caption safe-margin defaults only (no overlay guides in the viewer).

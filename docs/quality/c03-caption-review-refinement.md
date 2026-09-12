# C03 — Silence and filler review refinement

Implemented on branch `beta-1.0-fix` (phase 6 C03, commit `a84e638`).

## Behavior

- Review panel exposes breathing room (ms), protect program In/Out, and double-click audition before/after cuts.
- Silence proposals regenerate from measured ranges when options change; protected ranges are excluded from apply.
- Apply uses one coalescing key for timeline cuts plus caption additions; stale revision guard remains.

## Tests

- `EditorWindowTest::captionsPanelExposesTranscriptionAndWordNavigation` (review list affordances)

## Residual limitations

- Audition uses fixed preroll/postroll around the proposal range, not user-configurable audition handles.
- Protected range uses program In/Out only (not arbitrary marker ranges).

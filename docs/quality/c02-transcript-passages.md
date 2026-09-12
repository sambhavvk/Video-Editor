# C02 — Assemble selected transcript passages

Implemented on branch `beta-1.0-fix` (phase 6 C02, commit `80a3f2f`).

## Behavior

- Extended word list selection adds timed passages to an assembly list with reorder and clear.
- **Insert on timeline** ripple-inserts source-referenced clips in one undo batch (`insert-passages:` coalescing key).
- Passages map timeline word ranges back to source ranges via the active or transcription source clip.

## Tests

- Manual: select words → Add selection → Insert on timeline; verify undo and save/reopen of inserted clips.

## Residual limitations

- Assembly list is session-local (not persisted in the project file).
- Requires word timings and a resolvable source clip on the timeline.

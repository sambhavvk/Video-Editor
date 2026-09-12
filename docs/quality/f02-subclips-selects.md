# F02 — Subclips and selects sequences

## Automated evidence

- `F02SubclipsTest.SubclipPreservesSourceIdentityAndSelectsAssemblyIsUndoable` — subclip range/source identity, selects assembly, remove subclip leaves source asset, undo restores subclip.

## Manual verification

1. Select imported media and create a subclip over a source range (inspector or future range UI).
2. Add range notes and confirm media-bin search finds them.
3. Assemble a selects sequence from multiple subclips.
4. Save/reopen; verify subclip ranges and source asset ids persist.
5. Remove a subclip and confirm the original media file remains in the project.

## Limitations (F02)

- Subclip creation UI currently uses full-clip or API-driven ranges; in/out point marking from the viewer is a follow-up.

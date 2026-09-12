# C05 — Reviewable music ducking

Implemented on branch `beta-1.0-fix` (phase 6 C05, commit `57e00e3`).

## Behavior

- Audio mixer **Music ducking** group: dialogue track, threshold, depth, attack, release.
- **Generate on selected music clip** renders dialogue-track peaks and writes `audio.volume` keyframes on the active audio clip (one undo batch).
- Algorithm: `audio_render::generateMusicDuckingEnvelope`.

## Tests

- `MusicDucking.AttenuatesDuringDialogueAndRecoversAfter`
- `MusicDucking.NoDialogueKeepsUnityGain`

## Residual limitations

- Requires selecting the music clip before generate; no multi-clip batch ducking.
- Dialogue analysis uses track meters from full timeline render, not isolated stem export.

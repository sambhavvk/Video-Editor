# M04 — Timecode synchronization

## Automated evidence

- `MulticamTimecodeSyncTest.ProposesOffsetsFromEarliestSourceClock` — reads imported `timecode_start_us` metadata and proposes offsets against the earliest clock.

## Manual verification

1. Import clips with embedded timecode metadata.
2. Create a multicam group and choose **Propose timecode sync…** in the inspector.
3. Review the confirmation dialog; ambiguous/missing timecode angles require manual choice.
4. Apply and verify offsets update; undo restores prior sync.

## Limitations (M04)

- Waveform proposals are M05; drift correction remains out of scope.

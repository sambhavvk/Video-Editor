# M05 — Waveform synchronization proposals

## Automated evidence

- `MulticamWaveformSyncTest.RecoversKnownPositiveOffsetWithinTolerance` — bounded correlation finds a known 512-sample lag above the confidence floor.

## Manual verification

1. Create a multicam group with cached waveforms on the audio master and at least one other angle.
2. Choose **Propose waveform sync…**; low-confidence or silent matches should not apply silently.
3. Reject the dialog to keep manual offsets; apply to update offsets with undo support.

## Limitations (M05)

- Uses coarse waveform pyramid RMS buckets, not full long-recording drift correction.

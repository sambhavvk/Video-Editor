# F03 — Production-audio channel preservation

## Automated evidence

- `F03AudioChannelsTest.MonitoringSelectionValidatesChannelIndices` — multichannel labels persist on the asset model and monitoring indices are validated.

## Manual verification

1. Import a multichannel WAV/MP4 and confirm channel labels appear on the asset.
2. Set monitor left/right channels in the inspector and audition without re-encoding the source.
3. Save/reopen and verify channel map and monitoring selection return.

## Limitations (F03)

- Monitoring maps selected source channels to the existing stereo preview path; surround buses are out of scope.

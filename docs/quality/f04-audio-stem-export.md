# F04 — Production audio stem export

## Automated evidence

- `AudioStemExportTest.ReportsTrimmedHandles` — stem export reports when requested handles exceed available source media.

## Manual verification

1. Place dialogue on an audio track and set an export range with head/tail handles.
2. Export production stems to a folder and verify WAV sample counts match the declared range plus applied handles.
3. Cancel mid-export and confirm no partial stem is left without a warning.

## Limitations (F04)

- Stems are rendered through the 48 kHz stereo monitoring path; per-channel WAV stems are a follow-up.

# M03 — Four angles, switch edits, live cuts

## Automated evidence

- `MulticamM03Test.AllowsFourAnglesAndMissingAngleShowsBlack` — four-angle groups validate; disabled active angle yields no picture substitution.

## Manual verification

1. Select three or four video clips → **Create Multicam Group…**
2. Record switches, then adjust switch times via undo/redo of **Update multicam switch** edits (inspector cut buttons while playing with **Ctrl+1…4**).
3. Disable the active angle clip — preview shows black rather than another angle.

## Limitations (M03)

- Timecode and waveform sync proposals remain M04/M05.

# M02 — Angle selection at the playhead

## Automated evidence

- `MulticamM02Test.RecordsSwitchAtPlayheadAndSelectsActiveAngle` — switch recording, active-angle lookup, video visibility gating.

## Manual verification

1. Create a two-angle multicam group (M01).
2. Inspector shows two angle previews with **Cut to …** buttons; the active angle is highlighted at the playhead.
3. Move the playhead and click **Cut to Angle 2** — program preview switches picture; audio master stays unchanged.
4. Undo restores the prior switch; redo reapplies it.

## Limitations (M02)

- Switch point editing and four angles arrive in M03; live playback hotkeys while rolling arrive in M03.

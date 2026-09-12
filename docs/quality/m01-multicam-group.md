# M01 — Manually synchronized two-angle group

## Automated evidence

- `MulticamM01Test.CreateRemoveAndPersistTwoAngleGroup` — create, sync/audio/active updates, remove, undo.
- `MulticamM01Test.RejectsDuplicateClipMembership` — a clip cannot join two groups.
- `ProjectCodecTest.MulticamGroupsRoundTripInSchemaV6` — schema v6 protobuf round-trip.
- `ProjectCodecTest.RejectsMulticamGroupsInDeclaredSchemaV5` — declared v5 cannot smuggle multicam fields.

## Manual verification

1. Import or open a sequence with two video clips on separate tracks.
2. Select exactly two video clips → Timeline → **Create Multicam Group…**
3. Select a grouped clip → Inspector **Multicam group** shows angles, offsets, active angle, and audio master.
4. **Set sync point to playhead** stores offsets relative to the playhead; **Apply clip alignment** moves clips.
5. Save, close, and reopen — group, offsets, active angle, and audio master persist.
6. **Remove multicam group** and undo — group returns without deleting source clips.

## Limitations (M01)

- Exactly two angles per group; no switch recording yet (M02).
- Preview still composites all visible video tracks; active angle is editorial state only until M02.

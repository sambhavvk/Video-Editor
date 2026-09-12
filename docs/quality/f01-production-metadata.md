# F01 — Production metadata and saved views

## Automated evidence

- `F01ProductionMetadataTest.SceneTakeSearchAndRenamePreservesIdentity` — scene/take smart-query matching; rename keeps `source_uri` and `fingerprint`.
- `F01ProductionMetadataTest.SavedMediaViewRoundTripsThroughCodec` — saved views and active view persist through schema v7.

## Manual verification

1. Import media and open the inspector **Asset** group.
2. Enter scene, shot, take, camera, reel, audio roll, and source timecode; mark a preferred take.
3. Save/reopen the project and confirm production fields return.
4. Rename the clip title; relink checks still target the original file identity.
5. Use **Columns** to show production fields; **Save current view…** to store a named layout and search.
6. Create a smart bin filtered by scene/take and confirm matching clips appear.

## Limitations (F01)

- Source timecode display in the inspector is user-authored; probed `timecode_start_us` remains in technical metadata for sync tools.
- Saved views store a single text search token, not full smart-bin query UI.

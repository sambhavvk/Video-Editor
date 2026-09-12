# X04 — Aspect-ratio variant propagation (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X04). Discovery only; no implementation claimed.

## Options considered

| Option | Why not first |
| --- | --- |
| Drift / discontinuous-recording handling | Multicam waveform sync ([M05](m05-multicam-waveform-sync.md)) is coarse; full drift correction needs recording metadata not in model |
| Propagation between aspect-ratio variants | [C06](c06-aspect-ratio-copies.md) creates independent copies with explicit **no automatic sync** | **Selected** — closes documented C06 gap |

## Chosen design: optional edit propagation between linked aspect-ratio sequences

### Before / after examples

**Before (C06 today):**

- Active sequence `Interview` (any format). **Create Aspect-Ratio Copies…** adds two **new**
  sequences — `Interview (Landscape 16:9)` and `Interview (Vertical 9:16)` — via
  `duplicateSequenceWithNewIds`. The source sequence is left unchanged.
- Every track, clip, transition, marker, and caption ID is **reminted**. There is no stable
  `(track_id, clip_entity_id)` shared across copies.
- User ripple-trims 2 s on the landscape copy at 00:01:00:00.
- Vertical sequence unchanged → copies diverge silently (documented: "edits do not sync automatically").

**After (proposed v1):**

- C06 stores a `variant_group_id` plus an explicit **correspondence table** (source clip/track IDs →
  landscape IDs → vertical IDs) written in the same command batch. `variant_role` is
  `landscape` \| `vertical` on the copies. The original source sequence stays **out of the group**
  unless the user opts in (non-default).
- User chooses **Propagate trim to variants…** from a copy in the group (or enables
  **Auto-propagate structural edits** in sequence settings — still reviewable, never silent).
- Ripple trim on landscape generates a **reviewable propagation proposal** listing the vertical
  sibling with mapped timeline ranges.
- User applies → vertical receives equivalent ripple at mapped time; user discards → variants stay independent.

**Protected edits (never auto-applied):**

- Transform position/scale (reframing differs per aspect).
- Caption vertical position / safe margin overrides on vertical copy.
- Multicam angle switches, nested sequence content, speed/pitch, effects parameters.

**Propagated edit classes (v1):**

- Ripple/normal trim, split, delete, insert overwrite on targeted tracks when source time mapping is 1:1 across copies (same source clips, same rates).

### Source-time mapping

- At creation, copies share `asset_id` and source in/out on corresponding clips (parallel structure),
  but **not** identity: mapping must use the correspondence table, not live `track_id` / `clip.id`.
- Conflict: if the sibling clip was manually retimed, deleted, or unlinked, the proposal marks that
  item **blocked** with reason; user fixes manually.

### Model / UI / validation follow-ups

| Layer | Follow-up |
| --- | --- |
| Model | Schema bump after current snapshot **v7**: `VariantGroup` + correspondence ids; `PropagationProposal` with base revision + per-variant command batches |
| UI | Sequence header badge "2 variants"; Propagation panel listing pending/last sync; C06 dialog checkbox "Link for propagation" default on |
| Validation | Tests: trim propagates landscape→vertical; reframing change does not; blocked item when structure diverged |

## Non-goals (leave for later)

- Automatic drift correction for discontinuous multicam or field recordings.
- Tracked reframing (smart crop) between aspects.
- Shipping silent auto-propagation without review UI.
- Assuming C06 clip/track IDs are stable across copies (they are reminted today).
- Automatically linking the **source** sequence into the variant group.
- Propagation across sequence versions ([V01](v01-sequence-versions.md)) — versions remain compare-only until specified.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X04a | Schema `variant_group_id` + clip/track correspondence table written by C06 |
| X04b | Proposal builder for trim/split/delete |
| X04c | Review UI + apply batch + divergence detector |

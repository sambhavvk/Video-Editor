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

- Master `Interview` 16:9; user creates `Interview (Vertical 9:16)`.
- User ripple-trims 2 s from master at 00:01:00:00.
- Vertical sequence unchanged → versions diverge silently.

**After (proposed v1):**

- Copies created with C06 gain a `variant_group_id` (shared UUID) and `variant_role` (`landscape` \| `vertical`).
- User chooses **Propagate trim to variants…** (or enables **Auto-propagate structural edits** in sequence settings).
- Ripple trim on master generates a **reviewable propagation proposal** listing affected variants with mapped timeline ranges.
- User applies → vertical receives equivalent ripple at mapped time; user discards → variants stay independent.

**Protected edits (never auto-applied):**

- Transform position/scale (reframing differs per aspect).
- Caption vertical position / safe margin overrides on vertical copy.
- Multicam angle switches, nested sequence content, speed/pitch, effects parameters.

**Propagated edit classes (v1):**

- Ripple/normal trim, split, delete, insert overwrite on targeted tracks when source time mapping is 1:1 across copies (same source clips, same rates).

### Source-time mapping

- Variants share the same `asset_id` and source in/out on corresponding clips (C06 guarantees parallel structure at creation).
- Mapping key: `(track_id, clip_entity_id)` stable across copies created in one C06 command batch.
- Conflict: if vertical clip was manually retimed, proposal marks item **blocked** with reason; user fixes manually.

### Model / UI / validation follow-ups

| Layer | Follow-up |
| --- | --- |
| Model | `VariantGroup` in snapshot; `PropagationProposal` with base revision + per-variant command batches |
| UI | Sequence header badge "2 variants"; Propagation panel listing pending/last sync; C06 dialog checkbox "Link for propagation" default on |
| Validation | Tests: trim propagates; reframing change does not; blocked item when structure diverged |

## Non-goals (leave for later)

- Automatic drift correction for discontinuous multicam or field recordings.
- Tracked reframing (smart crop) between aspects.
- Shipping silent auto-propagation without review UI.
- Propagation across sequence versions ([V01](v01-sequence-versions.md)) — versions remain compare-only until specified.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X04a | Schema `variant_group_id`, C06 linkage metadata |
| X04b | Proposal builder for trim/split/delete |
| X04c | Review UI + apply batch + divergence detector |

# X08 — Unsaved session recovery (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X08). Discovery only; no implementation claimed.

## Options considered (X08 menu)

| Idea | FOSS path | Decision |
| --- | --- | --- |
| Open RAW (camera SDKs) | libraw (FOSS) vs vendor SDKs | **Deferred** — B02 not selected; patent/camera matrix large |
| HDR mastering | libplacebo tone-map partial | **Deferred** — beta matrix: HDR missing |
| Surround delivery | miniaudio stereo only | **Deferred** |
| Professional monitoring | HDMI LUT boxes | Hardware-dependent; **Deferred** |
| FOSS plugin host | LV2/VST3 FOSS hosts exist; ABI/security heavy | **Deferred** |
| Light / high-contrast theme | Qt stylesheets only | Smaller; see below as alternate |
| **Never-saved recovery** | Extends [ADR 0008](../architecture/0008-recovery-catalog.md) | **Selected remainder** — catalog already sees untitled working DBs |
| Self-hosted collaboration | Operational + sync model | **Deferred** — beta: collaboration deferred |
| Additional OS (macOS/ARM) | Packaging | Separate from feature; **Deferred** |

## Current product (do not rebuild the catalog)

Never-saved editing is **not** missing a working database:

- **File → New** writes `{projectUuid}.working.sqlite` under the platform recovery directory
  immediately (`EditorController::newWorkingPath`). Project display name is `Untitled Project`.
- Every committed edit appends a snapshot journal entry (`persistSnapshot`). Timed `.veproj`
  autosave still requires a saved checkpoint path (`beta-feature-status.md`).
- [ADR 0008](../architecture/0008-recovery-catalog.md) already recommends recovery when
  `clean_close` is false **or** `head_revision != saved_revision`. Tests cover a clean unsaved
  candidate. Restore Points lists those as **Unsaved edits** / **Unclean close**.
- [R03](r03-restore-points.md) residual is only "never-saved projects **without** a working
  database" — File → New always creates one.

Real gaps:

- Startup copy is generic ("Recover your last project?") and does not say the session was never
  saved to a `.veproj`.
- Discard on close (`confirmDiscardChanges`) marks `clean_close` but **does not delete** the
  untitled working DB, so the next launch can still offer it (`head != saved`).
- No retention/LRU cleanup (ADR 0008: cleanup is future policy).
- Switching File → New leaves the previous `{uuid}.working.sqlite` on disk (new UUID).

## Chosen: make never-saved recovery truthful and tidy

### User demand

Creators experiment before first **Save As**. Journal recovery already preserves those edits after
a crash; the demand is to **label**, **offer**, and **drop** never-saved candidates correctly
instead of inventing a second store.

### Acceptable open implementation

- Keep `{projectUuid}.working.sqlite` (do not rename to `unsaved-<uuid>` in v1; ADR scans
  `*.working.sqlite` children only).
- On crash (`clean_close = false`) or dirty untitled (`head != saved`), next launch offers
  **Recover unsaved session** (or the same dialog with that sentence) sorted by heartbeat as today.
- Accepting recovery stays read-latest-snapshot → dirty project; user must **Save As** (already
  `setDirty(true)` in `loadWorkingRecovery`).
- **Discard** on an untitled project with no `.veproj` path removes that working DB (and WAL/SHM)
  after `clean_close`, so it does not reappear.
- Retention: 7-day LRU for **orphan** recovery files that are clean, have no checkpoint, and are
  not the active working path.

### UI states

| State | Behavior |
| --- | --- |
| Editing unsaved | Title shows dirty `Untitled Project`; each edit already journals the working DB |
| First Save As | Existing `saveTo` binds `.veproj` and advances `saved_revision`; keep that path |
| Clean exit without save | Prompt: Discard / Save As / Cancel; Discard deletes the anonymous working candidate |
| Crash | `clean_close = false`; next launch shows a recovery entry labeled unsaved if no checkpoint path was bound |

### Sequence of small follow-up chunks

| ID | Work |
| --- | --- |
| X08a | Startup/restore copy distinguishes never-saved vs named checkpoint |
| X08b | Discard of untitled (no `.veproj`) deletes that working DB |
| X08c | 7-day LRU for orphan recovery files |
| X08d | Fault-injection test (process kill of untitled session) when protobuf lab available |

### Alternate quick win (not selected)

- **High-contrast theme**: Qt `Fusion` + stylesheet; no backend change. Logged as **X08-alt** if
  accessibility study prioritizes contrast over recovery.

## Deferred ideas (no FOSS path selected yet)

| Idea | Reason |
| --- | --- |
| Open RAW | libraw eligible but not in register; wide camera variance |
| HDR | Tone-map incomplete; no SDR/HDR export parity spec |
| Surround | Model + deliver recipes lack channel layouts |
| FOSS plugins | Sandboxing + ABI not designed |
| Collaboration | No sync protocol |

## Non-goals (leave for later)

- Treating the entire X08 menu as one task.
- Replacing ADR 0008 with a parallel `unsaved-*.working.sqlite` layout.
- Closed vendor monitoring SDKs or proprietary sync.
- macOS/Windows parity in the same chunk as recovery.
- Timed `.veproj` autosave without a save path (journal already covers crash durability).

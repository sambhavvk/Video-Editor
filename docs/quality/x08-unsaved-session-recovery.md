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
| **Never-saved recovery** | Extends [ADR 0008](../architecture/0008-recovery-catalog.md) | **Selected** |
| Self-hosted collaboration | Operational + sync model | **Deferred** — beta: collaboration deferred |
| Additional OS (macOS/ARM) | Packaging | Separate from feature; **Deferred** |

## Chosen: recover last **unsaved** editing session after crash

### User demand

- Creators frequently experiment before first **Save As**; today's recovery catalog targets
  `*.working.sqlite` tied to named projects ([ADR 0008](../architecture/0008-recovery-catalog.md)).
- Startup recovery ([beta-feature-status.md](../beta-feature-status.md)) handles saved checkpoints, not
  "never saved" windows.

### Acceptable open implementation

- Reuse working SQLite + snapshot journal: **anonymous session** writes to
  `recovery/unsaved-<uuid>.working.sqlite` from first edit (import or timeline change).
- Heartbeat + `clean_close` flag identical to named projects.
- On crash, next launch offers **Recover unsaved session** when no saved path exists or alongside
  named recovery (sorted by heartbeat).
- Accepting recovery opens read-only latest snapshot → dirty new project; user must **Save As**.
- Declining leaves files for manual inspection; retention policy (7-day LRU) in recovery dir.

### UI states

| State | Behavior |
| --- | --- |
| Editing unsaved | Title shows `*Untitled`; autosave tick writes anonymous working DB |
| First Save As | Migrate anonymous DB to chosen `.veproj` path; retire anonymous candidate |
| Clean exit without save | Prompt: Discard / Save As; discard marks `clean_close` and removes anonymous candidate |
| Crash | `clean_close = false`; next launch shows recovery dialog entry |

### Sequence of small follow-up chunks

| ID | Work |
| --- | --- |
| X08a | Anonymous session ID at first mutating command |
| X08b | Recovery catalog includes unsaved candidates + sort rules |
| X08c | Startup dialog + Save As migration |
| X08d | Fault-injection test (process kill) when protobuf lab available |

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
- Closed vendor monitoring SDKs or proprietary sync.
- macOS/Windows parity in the same chunk as recovery.

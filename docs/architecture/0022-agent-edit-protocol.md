<!-- SPDX-License-Identifier: MPL-2.0 -->

# Agent edit-command JSON protocol (P1)

Machine-facing edit commands are serialized as compact JSON objects in
`src/agent_protocol` (`video_editor_agent_protocol`). The codec uses Qt 6 Core
`QJsonDocument`; the edit model itself remains Qt-free.

## Wire format

Each command is a single JSON object:

```json
{
  "type": "insert_clip",
  "coalescing_key": "gesture-42",
  "sequence_id": "…",
  "track_id": "…",
  "clip": { "id": "…", "asset_id": "…", "kind": "video", … },
  "mode": "reject_overlap"
}
```

- **`type`** — stable snake_case id from `edit::commandType()`. This is the
  protocol discriminator. It is **not** `edit::commandName()`, which remains the
  English undo-history label for UI.
- **`coalescing_key`** — optional string; adjacent commands sharing a non-empty
  key collapse into one undo step (same semantics as `EditCommand::coalescing_key`).
- Remaining fields are the operation payload, flattened at the top level when
  possible. Nested model values (`clip`, `asset`, `marker`, `effect`, …) are JSON
  objects; lists are JSON arrays.

A batch is a JSON array of command objects. `encode_command_bytes` /
`decode_command_bytes` use compact JSON (no insignificant whitespace).

Schema reference: [`src/agent_protocol/schema/edit-command.v1.json`](../../src/agent_protocol/schema/edit-command.v1.json).

## Time convention

`Time` is lossless rational time, **not** OTIO floats:

```json
{ "value": 90000, "timescale": 30000 }
```

`Rate` uses `{ "numerator", "denominator" }`. This matches the edit model and
project snapshot v4 persistence; the agent protocol does not change on-disk
project format.

## Entity ids

- **New entities** (`Asset`, `Clip`, `Track`, `Sequence`, `MediaBin`, `Marker`,
  `Caption`, `Transition`, `Effect`, `CaptionWord`, `Keyframe`): UUID string in
  `id`. When `id` is missing or nil on decode, `EntityId::generate()` is used.
- **References** (`asset_id`, `sequence_id`, `clip_id`, …): required, non-nil,
  valid UUID; decode fails closed otherwise.

## Enums and tagged values

Enums are snake_case strings (`reject_overlap`, `video`, `cross_dissolve`, …).
`EffectValue` is a tagged object `{ "kind": "double", "value": 1.0 }` (or
`time` / `vec2` / `color` field layouts as documented in the codec).
Opaque effect bytes use `opaque_payload_base64`.

## API

```cpp
namespace video_editor::agent_protocol {
struct CodecError { std::string message; };

QJsonObject encode_command(const edit::EditCommand& command);
edit::Result<edit::EditCommand, CodecError> decode_command(const QJsonObject& object);

QByteArray encode_command_bytes(const edit::EditCommand& command);
edit::Result<edit::EditCommand, CodecError> decode_command_bytes(const QByteArray& bytes);
}
```

Unknown `type` strings fail closed with `CodecError`.

## Headless agent host (`video_editor_agent_host`)

The headless host executable lives in `src/agent_host`. It wraps
`edit::TimelineEditor`, optional `store::ProjectStore`, playback/render
services, and the JSON command codec behind a newline-delimited JSON (NDJSON)
transport on stdin/stdout.

### Transport

- **Default:** one JSON object request per input line; one JSON object response
  per output line.
- **`--once <request.json>`:** read a single request file, write the response to
  stdout, exit.

Each request includes `"method": "<name>"` plus method-specific parameters.
Responses are either `{ "ok": true, … }` or
`{ "ok": false, "error": { "code": "…", "message": "…" } }`. Edit failures also
include `expected_revision` / `actual_revision` when available. The `error.code`
for edit failures matches the `EditErrorCode` enumerator name (for example
`RevisionConflict`).

### Working database

The host uses a dedicated SQLite working file named
`{project_uuid}.agent.working.sqlite`, distinct from the desktop GUI recovery
file (`{id}.working.sqlite`). When opening a `.veproj` checkpoint, the host
copies it into that agent working path beside the checkpoint directory.

**Single-writer warning:** only one process should open a given
`.agent.working.sqlite` at a time. Do not `save_project` / desktop Save the
same `.veproj` from the GUI and agent host concurrently — both write the
checkpoint via `checkpoint_to`. Distinct working DB names avoid recovery-file
collisions; the checkpoint file itself remains one writer.

### Session state (not in snapshot v4)

Session fields are host-local and are not persisted in project snapshots:

- `playhead` — rational `Time` (default timescale 48000)
- `active_sequence_id`
- `selected_clip_ids`

### Methods

| Method | Purpose |
|--------|---------|
| `open_project` | Open a `.veproj`, hydrate the editor from the latest `project.snapshot.v1`–`v4` journal entry, register assets for playback |
| `new_project` | Create an in-memory 1920×1080 project with `Sequence 1`, `V1`, and `A1` |
| `save_project` | Append `project.snapshot.v4` and checkpoint to `.veproj` (creates a store on first save) |
| `read_session` | Return revision, session fields, and a compact project summary |
| `set_session` | Update playhead / active sequence / selection without bumping edit revision |
| `apply_commands` | Decode and `applyBatch` agent-protocol commands; persist when a store is open |
| `undo` / `redo` | History navigation with the same persist rules |
| `import_media` | `AssetService::import`, `AddAssetCommand`, registry registration |
| `insert_media` | Import-or-use asset, mirror desktop insert-at-playhead semantics (including still-overlay ripple handling) |
| `get_preview_frame` | CPU render at playhead (or requested time) to PNG |
| `get_scopes` | `ScopeAnalyzer` summary or full scope arrays |

`expected_revision` is optional on mutating calls; when omitted the host uses the
current `TimelineEditor` revision.

### Tests

`tests/agent_host/agent_host_test.cpp` exercises `AgentHost` directly (no stdin),
including import/insert/preview, `apply_commands` round-trip, and unknown-method
errors.

## Scope and deferrals

- **In scope:** JSON encode/decode for all 53 `EditOperation` alternatives,
  round-trip tests, JSON Schema listing command types, headless agent host with
  NDJSON transport and preview/scope tools.
- **Out of scope (deferred):** desktop UI wiring to spawn or supervise the agent
  host. Snapshot v4 / SQLite persistence format is unchanged.

## Tests

`tests/agent_protocol/edit_command_json_test.cpp` round-trips every command type,
verifies unknown types fail, and checks omitted `Clip.id` generates a non-nil id.

# X07 — Transcript topic search (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X07). Discovery only; no implementation claimed.

## Options considered

| Option | Why not first |
| --- | --- |
| Transcript-based topic search | [C01](c01-transcript-navigation.md) has per-cue UTF-8 search only | **Selected** — extends shipped captions |
| Source-backed B-roll suggestions | Needs vision embeddings + media understanding | Heavy model scope |
| Editable chapter/excerpt drafts | New document type | Larger UX |

## Chosen workflow: local topic / phrase search over timed words

### Inputs

- All `CaptionWord` records in the active sequence (import, transcription, user-edited text preserved).
- Optional scope: entire sequence or playhead-selected range.
- Query: UTF-8 phrase or comma-separated keywords (v1); no cloud query.

### FOSS-eligible local approach

| Tier | Method | Runtime / assets |
| --- | --- | --- |
| **v1 (ship first)** | Case-insensitive substring + word-token AND over timed words | O(words); no new model |
| **v2 (prototype)** | BM25 over cue text in-process (MPL-2.0) | Still no ML weights |
| **Rejected** | Proprietary LLM APIs, ChatGPT, Gemini | Non-FOSS / network |
| **Rejected v1** | Local LLM (llama.cpp) for "topics" | Large weights; user didn't ask for generative |
| **Deferred** | `whisper.cpp` embedding head | Not in pinned 1.9.2 contract |

Runtime check before v2: benchmark 10k-word transcript search < 50 ms on reference laptop.

### Source / time references

- Each hit returns `(cue_id, word_entity_id, timeline_start, timeline_end, snippet)`.
- **Go to** seeks word start ([C01](c01-transcript-navigation.md) behavior).
- Search does not mutate captions ([ADR 0018](../architecture/0018-local-transcription-and-caption-proposals.md)).

### Uncertainty / review UI

- Results list with match score (v1: match count / first offset).
- No auto-edits; optional **Select hits** → filter caption panel rows.
- Empty state: "No matches in sequence" with link to widen scope.

### Manual alternative

- Existing caption search box (per-cue filter) remains; topic search is global navigator with **Next/Previous hit** (addresses C01 residual).

## Non-goals (leave for later)

- B-roll suggestions, generative chapter drafts, diarization.
- Proprietary AI APIs or restricted model weights.
- Promises of "viral" clip detection.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X07a | `TranscriptSearchIndex` build from snapshot words |
| X07b | Captions panel search mode + hit navigation |
| X07c | Tests on imported SRT + transcribed words |

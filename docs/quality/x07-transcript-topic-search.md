# X07 — Transcript topic search (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X07). Discovery only; no implementation claimed.

## Options considered

| Option | Why not first |
| --- | --- |
| Transcript-based topic search | [C01](c01-transcript-navigation.md) filters cue rows; no next/previous navigator | **Selected** — extends shipped captions |
| Source-backed B-roll suggestions | Needs vision embeddings + media understanding | Heavy model scope |
| Editable chapter/excerpt drafts | New document type | Larger UX |

## Chosen workflow: local topic / phrase search over timed words

### Inputs

- All `CaptionWord` records in the active sequence (import, transcription, user-edited text preserved).
- Optional scope: entire sequence or playhead-selected range.
- Query: UTF-8 phrase or comma-separated keywords (v1); no cloud query.

Today [C01](c01-transcript-navigation.md) already runs `caption_service::search` over cue text
(case-insensitive UTF-8 substring, optional word-id mapping) and **filters caption rows**. Residual:
no global next/previous hit navigator, no keyword AND, no range-restricted topic query.

### FOSS-eligible local approach

| Tier | Method | Runtime / assets |
| --- | --- | --- |
| **v1 (ship first)** | Extend `caption_service::search` with case-insensitive substring + word-token AND; Next/Previous hit | O(words); no new model |
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

- Existing caption search box (per-cue filter) remains; topic search adds **Next/Previous hit** on
  the same `caption_service::search` hits (addresses C01 residual). Do not stand up a second
  parallel search engine in v1.

## Non-goals (leave for later)

- B-roll suggestions, generative chapter drafts, diarization.
- Proprietary AI APIs or restricted model weights.
- Promises of "viral" clip detection.
- Replacing `caption_service::search` with a separate persisted index in v1.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X07a | Keyword AND + range scope on existing `caption_service::search` |
| X07b | Captions panel search mode + hit navigation |
| X07c | Tests on imported SRT + transcribed words |

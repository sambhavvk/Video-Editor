<!-- SPDX-License-Identifier: MPL-2.0 -->

# CaptionService API reference

Header: `video_editor/caption_service/caption_service.h`

Namespace: `video_editor::caption_service`

## Documents and exchange

`CaptionDocument` owns one normalized SRT or WebVTT document. Each `CaptionCue` owns an optional
identifier, exact half-open `TimeRange`, UTF-8 text, preserved WebVTT/SRT settings, source line,
canonical timed words, and provenance.

`parse` and the format-specific wrappers validate UTF-8, timing, order, overlap, identifiers, and
cue text. `serialize` normalizes line endings and requires exact milliseconds unless the caller
selects explicit nearest-millisecond rounding. Styling and word timing are canonical project data;
plain SRT and WebVTT sidecars do not preserve every editor-only field.

## Reflow and transcript navigation

`reflow(document, options)` wraps on word boundaries and splits cues that exceed the line limit.
When timed words are present and `preserve_word_timing` is enabled, cue boundaries are derived from
the words instead of text-length interpolation. Word timing, probability, IDs, and provenance stay
attached to their resulting cue.

`search` returns exact cue time and UTF-8 byte offsets. When a hit resolves to a canonical word, the
`SearchHit` also carries its stable word ID and exact word range, allowing the controller to seek
without recomputing timing from displayed text.

`toEditCaption`/`fromEditCaption` and their range forms copy timed words, provenance, and style
between exchange documents and the canonical edit model. Returned values own all data.

## Review proposals

`CaptionProposal` binds review items and complete edit-model change sets to `base_revision`.
`buildTimelineCutProposal(snapshot, ranges)` accepts sorted, non-overlapping, positive exact ranges
and materializes deterministic clip fragments for every affected media track. It preserves source
mapping, playback rate/reverse behavior, linked A/V grouping, and unchanged clip IDs. A locked
affected track or unsupported transition relationship fails before any command is published.

The function only prepares values. A controller must show the items, retain the proposal revision,
and submit selected caption and timeline changes through `TimelineEditor::applyBatch` with that
revision as `expected_revision`. Stale, invalid, or rejected proposals do not mutate the project.

Measured-silence ranges come from `audio_render::detectSilence`; transcript filler/gap proposals
must be labelled separately because text timing alone is not an audio measurement.

## Thread safety

All functions are stateless or operate on caller-owned values. Independent calls may run in
parallel. A `TimelineSnapshot` is immutable for the lifetime of proposal planning. No returned
object borrows parser input, snapshot storage, or temporary strings.

AI assistance has been used to create this output.

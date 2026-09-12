# R02 — Background job admission during editing

Implemented on branch `beta-1.0-fix` (phase 4 R02).

## Behavior

- Proxy and media-cache (thumbnail/waveform) **admission** pauses while transport is running or
  while the user is scrubbing (timeline/source seek with a short debounce before resume).
- In-flight workers are not preempted; cancellation remains explicit (proxy toggle-cancel, project
  reload cache cancel).
- Status bar job summary reports paused state, queued counts, active work, and a rolling failure
  count. Hover the label for persistent proxy/cache failure details.

## Tests

- `EditorControllerTest::backgroundJobsPauseDuringPlayback`

## Residual limitations

- Export and transcription jobs are reported but not gated by the admission pause.
- Normalization and caption silence analysis (QtConcurrent) are outside the admission gate.
- No dedicated jobs panel; failures are capped at 16 entries in the status tooltip.

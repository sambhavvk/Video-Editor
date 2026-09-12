# D03 — Consolidated export settings

Implemented on branch `beta-1.0-fix` (phase 5 D03).

## Behavior

- Deliver panel **delivery overview** summarizes preset, destination, size, frame rate, Rec.709
  limited color, audio codec, captions, In/Out range, original-media source, and encoder
  availability in one line.
- Existing export queue, cancellation, and encoder capability summary are unchanged.

## Residual limitations

- Overview does not list per-job progress (see export queue when enabled).

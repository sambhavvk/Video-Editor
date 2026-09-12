# E01 — Selection and track destinations

Implemented on branch `beta-1.0-fix` (phase 3 E01).

## Visual distinctions

- **Active clip:** gold top/bottom bars on the primary edit target.
- **Selected clips:** bright outline plus side accent bars that stay legible over custom clip colors.
- **Linked companions:** teal dashed outline when linked selection is on and another member of the
  same linked group is selected.
- **Targeted tracks:** teal left rail on the row body and header.
- **Locked tracks:** warm header tint, diagonal hatch overlay, and muted header text.
- **Untargeted tracks:** slightly dimmed row body so insert destinations stand out.
- **Invalid move preview:** red translucent preview when dragging onto a locked destination track.

## Rejection messages

Locked-track razor, trim, envelope, and nudge attempts, plus moves onto locked tracks, emit a direct
status-bar message through `TimelineWidget::editDestinationRejected`. Model batch failures map locked,
overlap, and targeting errors to user-directed copy instead of raw model strings.

## Tests

- `EditorWindowTest::timelineRejectsLockedTrackEditsWithMessage`
- `EditorWindowTest::timelineShowsLinkedCompanionSelection`

<!-- SPDX-License-Identifier: MPL-2.0 -->

# TimelineWidget

Header: `video_editor/desktop_ui/timeline_widget.hpp`

Namespace: `video_editor::desktop_ui`

## Class overview

`TimelineWidget` is the virtualized Qt Widgets timeline surface. It paints tracks, clips, markers,
gaps, transient edit previews, snap guides, and focus feedback without creating a child widget per
clip. When `TimelineClipView.waveform` is populated, the lower portion of a sufficiently large clip
draws a static min/max waveform; PROXY and Media offline badges remain. It owns transient
selection/gesture state and emits presentation intents; the application controller performs exact
model mutation and refreshes authoritative views.

## Project structure and dependencies

The class is constructed and exposed by `EditorWindow`; `EditorController` supplies view models,
connects the rich signals, and installs the exact snap resolver. The desktop target requires
`Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets`. `ui_types.hpp` supplies every presentation value.

## Class hierarchy and role

`TimelineWidget` is a final `QAbstractScrollArea`. The base provides a scrollable viewport,
horizontal/vertical scrollbars, focus, events, and QWidget parent ownership. Painting and hit
testing operate only on the visible viewport.

## Properties

| Property | Type | READ | WRITE | NOTIFY | Description |
| --- | --- | --- | --- | --- | --- |
| `pixelsPerSecond` | `double` | `pixelsPerSecond` | `setPixelsPerSecond` | `zoomChanged` | Timeline zoom, bounded to the supported virtualized range. |
| `playhead` | `qint64` | `playhead` | `setPlayhead` | `playheadChanged` | Current playhead in the supplied integer timescale. |

## Enumerations

| Enum | Values | Description |
| --- | --- | --- |
| `ClipHitRegion` | `None`, `Body`, `TrimIn`, `TrimOut`, `Envelope`, `EnvelopeKeyframe` | Pointer location relative to a clip, its edge handles, or its volume/opacity rubber-band. |
| `EditMode` | `Move`, `TrimIn`, `TrimOut`, `Roll`, `Slip`, `Slide` | Geometry intent emitted for a completed gesture. |
| `ToolMode` | `Select`, `RippleTrim`, `OverwriteTrim`, `Roll`, `Slip`, `Slide`, `TrackSelectForward`, `Razor`, `Hand`, `Zoom`, `Pen` | Persistent active timeline tool. Ripple/overwrite/roll require an edge; slip/slide use a body; razor splits on click; pen edits volume and opacity envelopes; hand pans; zoom click/drags around the pointer. The viewport cursor is a drawn professional cursor for the tool and hit region. |
| `EditIntent` | `Normal`, `Ripple`, `Overwrite` | Overlap/trim policy requested from the controller. |

## Public methods

### `explicit TimelineWidget(QWidget* parent = nullptr)`

Creates a strongly focusable, accessible timeline owned by `parent` when supplied.

### `void setTimeline(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks, QVector<TimelineClipView> clips)`

Replaces the basic authoritative view and clears marker/gap inputs through the compatibility path.

### `void setTimeline(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks, QVector<TimelineClipView> clips, QVector<TimelineMarkerView> markers, QVector<TimelineGapView> gaps)`

Replaces the complete authoritative timeline view. An active gesture is canceled before refresh.

### `void setTracks(QVector<TimelineTrackView> tracks)`

Replaces painted track/header state and updates scroll extents.

### `void setClips(QVector<TimelineClipView> clips)`

Replaces clip geometry while retaining compatible transient selection IDs.

### `void setDuration(qint64 duration, qint64 timeScale)`

Sets non-negative duration and positive integer timescale and clamps the playhead.

### `void setMarkers(QVector<TimelineMarkerView> markers)` / `void setGaps(QVector<TimelineGapView> gaps)`

Replace marker or derived-gap presentation values.

### `void setSnapResolver(std::function<TimelineSnapResult(const TimelineSnapRequest&)> resolver)`

Installs the synchronous GUI-thread resolver. The callback must not retain request references or
perform blocking work; the controller converts to exact edit-model time and returns the canonical
candidate. Clip gestures list every moving clip in `excludedClipIds`; marker moves place the active
marker in `excludedMarkerId`, preventing either interaction from snapping back to itself.

### Query methods

`duration()`, `timeScale()`, `playhead()`, `pixelsPerSecond()`, `tracks()`, `clips()`, `markers()`,
`gaps()`, `selectedClipIds()`, `activeClipId()`, `toolMode()`, `visibleClipCount()`,
`snapThresholdPixels()`, `frameRateNumerator()`, and `frameRateDenominator()` expose current
presentation state. `frameStep()` is a compatibility display increment; canonical editing nudges
emit frame counts. `clipHitRegionAt()` performs viewport-coordinate hit testing.

## Public slots

### `void setPlayhead(qint64 position)`

Clamps, reveals, repaints, and emits `playheadChanged` when the value changes.

### `void setPixelsPerSecond(double pixelsPerSecond)`

Changes zoom around the visible center and emits `zoomChanged`.

### `void zoomIn()` / `void zoomOut()` / `void zoomToFit()`

Apply standard timeline zoom operations.

### `void setSnapThresholdPixels(int threshold)`

Sets the 0–64 pixel resolver threshold; zero disables snapping.

### `void setFrameRate(quint32 numerator, quint32 denominator)`

Sets the display/nudge frame-rate metadata with nonzero components.

### `void setToolMode(ToolMode mode)`

Cancels an active clip or pan gesture, changes the tool, emits `toolModeChanged`, and refreshes the
professional viewport cursor.

### `void setTrackHeight(int pixels)`

Sets the uniform clip-track row height used for envelope editing. Values are clamped to 36–160 px.

### `TimelineCursorKind hoverCursorKind(const QPoint& viewportPosition, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const`

Returns the cursor that `updateHoverCursor` applies: razor/hand/zoom follow the tool even on empty
canvas; trim tools use dual-arrow variants on edges; select uses a move hand on clip bodies.

### `void nudgeActiveClipByFrames(int frameCount, EditIntent intent = EditIntent::Normal)`

Emits `frameNudgeRequested` for the current selection. Rich controller clients resolve the frame
count exactly; compatibility single-clip signals remain available for older callers.

## Signals

### `void seekRequested(qint64 position)`

Requests an authoritative seek after ruler/keyboard playhead interaction.

### `void clipActivated(const QString& clipId)`

Identifies the active clip for inspector presentation.

### `void clipContextMenuRequested(const QString& clipId, const QPoint& globalPosition)`

Requests application-specific clip context actions at a global screen position.

### `void playheadChanged(qint64 position)` / `void zoomChanged(double pixelsPerSecond)`

Notify observers after the corresponding property changes.

### `void clipEditPreview(...)`, `void clipEditCommitted(...)`, `void clipEditCanceled(...)`

Compatibility single-clip gesture signals. New controllers should connect only the rich batch
signals to avoid duplicate edits.

### `void clipSelectionChanged(const QStringList& clipIds, const QString& activeClipId)`

Emitted after replace, Control-toggle, or Shift-range selection. The controller mirrors this
transient state and prunes it against revisions.

### `void clipBatchEditPreview(const QStringList& clipIds, int destinationTrackIndex, qint64 startDelta, qint64 durationDelta, EditMode mode, EditIntent intent, const TimelineSnapResult& snap)`

Reports transient multi-clip geometry during a valid drag. It must not mutate the project.

### `void clipBatchEditCommitted(const QStringList& clipIds, int destinationTrackIndex, qint64 startDelta, qint64 durationDelta, EditMode mode, EditIntent intent, const TimelineSnapResult& snap)`

Requests one exact command or atomic batch when the pointer is released.

### `void clipBatchEditCanceled(const QStringList& clipIds)`

Requests restoration of authoritative geometry after Escape/cancellation without a revision.

### `void frameNudgeRequested(const QStringList& clipIds, int frameCount, EditIntent intent)`

Requests exact frame-rate movement for the current selection.

### `void clipAudioGainEdited(const QString& clipId, double gainDb)` / `void clipOpacityEdited(const QString& clipId, double opacity)`

Commit a rubber-band drag that changes clip gain or transform opacity without keyframes.

### `void clipVolumeKeyframeUpserted(const QString& clipId, const QString& keyframeId, qint64 localTime, double gainDb)` / `void clipVolumeKeyframeRemoved(const QString& clipId, const QString& keyframeId)`

Pen/Ctrl envelope edits on audio clips. An empty keyframe ID creates `audio.volume` as needed.

### `void clipOpacityKeyframeUpserted(const QString& clipId, const QString& keyframeId, qint64 localTime, double opacity)` / `void clipOpacityKeyframeRemoved(const QString& clipId, const QString& keyframeId)`

Pen/Ctrl envelope edits on video clips. An empty keyframe ID creates `video.opacity` as needed.

### `void clipFreezeFrameRequested(const QString& clipId, qint64 uiTime)`

Requests a hold of the source frame at `uiTime` for the rest of the clip.

### `void markerSelectionChanged(const QString& markerId)`

Selects one marker exclusively; an empty ID clears marker selection.

### `void markerMovePreview(const QString& markerId, qint64 start, const TimelineSnapResult& snap)`

Reports transient marker placement.

### `void markerMoveCommitted(const QString& markerId, qint64 start, const TimelineSnapResult& snap)`

Requests one canonical marker update.

### `void markerMoveCanceled(const QString& markerId)`

Reports Escape cancellation of marker drag.

### `void markerAddRequested(qint64 start)`

Requests a point marker at a ruler double-click or the M shortcut.

### `void markerRenameRequested(const QString& markerId, const QString& displayName)` / `void markerRemoveRequested(const QString& markerId)`

Request context-menu marker mutation.

### `void gapSelectionChanged(const QString& gapKey)` / `void closeGapRequested(const QString& gapKey)`

Select a revision-local derived gap or request exact re-resolution and closure.

### `void trackAddRequested(TrackKind kind)`

Requests a new canonical track from the context menu or keyboard shortcut.

### `void trackRenameRequested(const QString& trackId, const QString& displayName)`

Requests a validated track-name replacement.

### `void trackReorderRequested(const QString& trackId, int destinationIndex)`

Requests movement to a zero-based track position.

### `void trackLockToggled(const QString& trackId, bool locked)` / `void trackVisibilityToggled(const QString& trackId, bool visible)` / `void trackTargetToggled(const QString& trackId, bool targeted)`

Request canonical header-state changes. Painted text/icons supplement color feedback.

### `void trackRemoveRequested(const QString& trackId)`

Requests removal only after an explicit context-menu choice; opening the menu itself is harmless.

## Protected event handlers

`paintEvent` draws only visible tracks/clips plus ruler, markers, gaps, guides, previews, headers,
envelopes, and focus. `resizeEvent` updates scroll ranges. `wheelEvent` supports zoom, horizontal
scroll, and track-height changes when the pointer is over a track header.
Mouse press/move/release implement selection and cancelable gestures; double-click adds markers;
`contextMenuEvent` exposes non-destructive menus; `keyPressEvent` supports Escape, playhead movement,
frame nudging, markers, gaps, track creation and reordering, and track lock/visibility/target toggles.

## Ownership, lifecycle, and thread safety

The parent widget owns `TimelineWidget`. View vectors and resolver function are owned by value. The
resolver commonly captures `EditorController`, so controller/window teardown must prevent calls
after controller destruction. Like every QWidget, all methods and signals are GUI-thread only.

## Usage example

```cpp
auto *timeline = new desktop_ui::TimelineWidget(parent);
timeline->setSnapResolver([controller](const auto& request) {
  return controller->resolveTimelineSnap(request);
});
QObject::connect(timeline, &desktop_ui::TimelineWidget::clipBatchEditCommitted,
                 controller, &EditorController::commitTimelineBatchEdit);
```

AI assistance has been used to create this output.

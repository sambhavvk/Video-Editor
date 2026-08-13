<!-- SPDX-License-Identifier: MPL-2.0 -->

# KeyframeCurveWidget

Header: `video_editor/desktop_ui/keyframe_curve_widget.hpp`

Namespace: `video_editor::desktop_ui`

## Class overview

`KeyframeCurveWidget` is a compact Qt Widgets editor for one presentation-level effect curve. It
owns a copy of the supplied `KeyframeView` values and emits preview or commit intent; it never
mutates an edit-model snapshot.

## Public API

### `explicit KeyframeCurveWidget(QWidget* parent = nullptr)`

Creates a focusable, accessible curve surface with mouse and keyboard interaction.

### `void setKeyframes(const QVector<KeyframeView>& keyframes, qint64 duration, double minimum, double maximum)`

Replaces the local curve, its clip-local duration, and visible value range. Duration is clamped to
at least one presentation tick and reversed value limits are normalized.

### `void setSelectedKeyframe(const QString& keyframeId)` / `QString selectedKeyframeId() const`

Changes or reads the presentation selection. The identifier is owned by the widget.

## Signals and gestures

`keyframeSelected` reports point selection. `keyframeValuePreview` reports transient point drags.
`keyframeValueCommitted` reports one completed point edit. `keyframeControlPointsCommitted` reports
the selected incoming/outgoing Bezier offsets after a handle drag.

A left drag edits a keyframe or visible Bezier handle. Mouse release commits once. Escape during a
drag restores its original time, value, and handles without a commit. Arrow keys adjust the selected
point and commit; Tab selects the first point when none is active.

## Ownership and thread safety

The widget is QObject-parent-owned and GUI-thread only. All keyframe data is copied. Receivers must
convert presentation ticks and values into a revision-checked edit command.

AI assistance has been used to create this output.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# Exact snapping API reference

Header: `video_editor/edit_model/snapping.h`

Namespace: `video_editor::edit`

## Overview

The snapping API is a pure exact-time query used by the controller-backed timeline resolver. It
never converts to floating point and never mutates the sequence.

## Enumerations

| Enum | Values | Description |
| --- | --- | --- |
| `SnapTargetKind` | `Playhead`, `Marker`, `ClipEdge`, `FrameGrid` | Candidate source and stable equal-distance priority order. |
| `SnapEdge` | `None`, `Start`, `End` | Identifies a range boundary where applicable. |

## Types

| Type | Fields | Description |
| --- | --- | --- |
| `SnapRequest` | proposed exact time, non-negative threshold, optional playhead, include flags, `excluded_clip_ids`, `excluded_marker_ids` | Selects candidate families and omits moving clips and markers from their own boundary targets. Each exclusion applies to both boundaries. |
| `SnapCandidate` | exact target time/distance, kind/edge, optional entity/track/frame identity | Complete deterministic snap result. |

## Functions

### `std::vector<SnapCandidate> findSnapCandidates(const Sequence& sequence, const SnapRequest& request)`

Returns every candidate inside the inclusive threshold, sorted by distance, target-kind priority,
time, entity identity, track identity, edge, and frame number. A negative threshold throws
`std::invalid_argument`.

### `std::optional<SnapCandidate> nearestSnapCandidate(const Sequence& sequence, const SnapRequest& request)`

Returns the first canonical candidate or no value when nothing is within threshold.

## Thread safety

Calls share no mutable global state. Independent callers may query immutable sequences
concurrently. The caller must synchronize access to a sequence that it mutates elsewhere.

## Usage example

```cpp
edit::SnapRequest request;
request.proposed_time = edit::Time{1, 30};
request.threshold = edit::Time{1, 30'000};
request.excluded_clip_ids.insert(moving_clip_id);
request.excluded_marker_ids.insert(dragged_marker_id);

const auto snap = edit::nearestSnapCandidate(sequence, request);
```

AI assistance has been used to create this output.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# Desktop presentation types

Header: `video_editor/desktop_ui/ui_types.hpp`

Namespace: `video_editor::desktop_ui`

## Overview

These owned Qt value types cross the controller/presentation boundary. Timeline times are integer
values in the `TimelineWidget` timescale supplied alongside the view. No edit-model type leaks into
the desktop library.

## Enumerations

| Enum | Values | Description |
| --- | --- | --- |
| `Workspace` | `Import`, `Edit`, `AudioCaptions`, `Deliver` | Active progressive-disclosure desktop layout. |
| `TrackKind` | `Video`, `Audio`, `Caption` | Presentation track category. |
| `TimelineSnapKind` | `None`, `Frame`, `Marker`, `Playhead`, `ClipEdge` | Canonical resolver result used for guide styling and accessible text. |
| `KeyframeInterpolationView` | `Hold`, `Linear`, `Bezier` | Presentation form of the canonical interpolation policy. |
| `TranscriptionState` | `ModelMissing`, `Downloading`, `Ready`, `Running`, `Cancelling`, `Failed` | Truthful model/job state shown by the captions panel. |

## Types

| Type | Significant fields | Description |
| --- | --- | --- |
| `MediaItemView` | identity/name/path/duration/format plus offline/proxy flags | One searchable media-bin row. |
| `TimelineTrackView` | identity/name/kind plus mute/solo/lock/visible/targeted | One painted track header and its output/editor state. Visibility and targeting default true. |
| `TimelineMarkerView` | identity/name/start/duration/color/selected | Point or ranged marker on the ruler. |
| `TimelineGapView` | revision-local key, track identity/index, start/duration, selected | Derived empty timeline range; the key must be re-resolved by the controller before editing. |
| `TimelineSnapRequest` | proposed time, pixel threshold, excluded clip IDs, optional excluded marker ID, marker flag | Presentation request passed synchronously to the controller's exact snap resolver. A marker drag supplies its own ID; marker creation leaves the field empty. |
| `TimelineSnapResult` | resolved time, kind, label | Resolver response. `snapped()` is true for every kind except `None`. |
| `AudioTrackView` | stable ID, display name, mute/solo, gain/pan, meter state, current typed effect values | One mixer strip's authoritative presentation state. Nested effects preserve stable IDs and current parameters across rebuilds. |
| `AudioTrackMeterView` | stable track ID, stereo peak/RMS, active/stale flags | One audio-master-position meter update; identity prevents reorder/removal misrouting. |
| `TimelineClipView` | identity/name, track/start/duration, color, selection/offline/proxy flags | One virtualized painted clip. |
| `EffectView` | identity/name/category/accelerated | One effects-browser row. |
| `KeyframeView` | identity, clip-local time/value, interpolation, incoming/outgoing offsets | One editable curve sample. |
| `EffectParameterView` | effect/parameter identity and names, base value, clip duration, keyframes | Complete Inspector view for one effect parameter. |
| `CaptionWordView` | stable ID, text, start/end ticks, probability | One exact word-navigation target. |
| `CaptionStyleView` | font/size/colors/emphasis, alignment, vertical position, safe margin, outline | Complete renderer-actionable caption presentation style. |
| `CaptionRowView` | stable cue ID, time/text/language/range, words/style, confidence/suggested | One caption-table row and its word/style detail state. |
| `TranscriptionOptionsView` | model/language, translate, GPU preference, required word timing, thread count | User-requested local worker options; it does not imply backend availability or runtime Vulkan use. |
| `ModelDownloadView` | model/filename/digest, byte progress, status/state | Explicit checksummed model acquisition state. |
| `CaptionProposalView` | stable ID, kind, summary, range/confidence text, selected | One independently reviewable caption or timeline-cut suggestion. |

## Ownership and thread safety

Every type owns its strings, lists, colors, and scalar data. The GUI/controller copies or moves
these values on the Qt GUI thread. They contain no synchronization and should not be mutated from a
worker while a widget reads them.

## Usage example

```cpp
desktop_ui::TimelineTrackView track{
    .id = QStringLiteral("video-1"),
    .displayName = QStringLiteral("V1 Primary"),
    .kind = desktop_ui::TrackKind::Video,
    .visible = true,
    .targeted = true,
};
```

AI assistance has been used to create this output.

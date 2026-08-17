<!-- SPDX-License-Identifier: MPL-2.0 -->

# Canonical project model API reference

Header: `video_editor/edit_model/model.h`

Namespace: `video_editor::edit`

## Overview

This header defines the dependency-free, authoritative values stored in immutable project
revisions. Values use ordinary C++ ownership and contain no Qt, FFmpeg, SQLite, Protobuf, or GPU
types. Callers may prepare mutable values privately; values reached through a `TimelineSnapshot`
are immutable and may be read concurrently for the snapshot lifetime.

## Enumerations

| Enum | Values | Meaning |
| --- | --- | --- |
| `TitleHorizontalAlignment` | `Left`, `Center`, `Right` | Horizontal title layout inside the sequence canvas. |
| `KeyframeInterpolation` | `Hold`, `Linear`, `Bezier` | Parameter interpolation after a keyframe. |
| `TrackKind` | `Video`, `Audio`, `Caption` | Determines the content accepted by a track. Caption entities remain sequence-owned. |
| `ClipKind` | `Video`, `Audio`, `Title` | Determines media/title validation and render behavior. |
| `BlendMode` | `Normal`, `Add`, `Multiply`, `Screen`, `Overlay` | Supported visual compositing operation. |
| `TransitionKind` | `CrossDissolve`, `DipToBlack` | Supported CPU-reference transition. |
| `CaptionAlignment` | `Left`, `Center`, `Right` | Horizontal caption placement inside its safe area. |
| `CaptionWordSource` | `Unknown`, `Imported`, `LocalTranscription`, `UserEdited` | Provenance category for exact word timing. |

## Primitive and effect values

| Type | Significant fields | Description |
| --- | --- | --- |
| `Revision` | `value` | Monotonic project-head identity. |
| `Vec2` | `x`, `y` | Two-component numeric value used by transforms and curve controls. |
| `ColorRgba` | `red`, `green`, `blue`, `alpha` | Straight normalized color channels. |
| `Title` | `text`, `font_family`, `font_size`, foreground/background colors, alignment, `bold`, `italic` | Complete persistent title payload. |
| `EffectValue` | Variant of integer, double, bool, string, `Time`, `Vec2`, `ColorRgba` | Typed effect value storage. |
| `Keyframe` | `id`, clip-local `time`, `value`, interpolation, incoming/outgoing control offsets | One exact-time parameter sample. Incoming X points left and outgoing X points right from the owning keyframe. |
| `EffectParameter` | `id`, `value`, `keyframes` | Current parameter value and its curve. |
| `Effect` | `id`, `type`, `version`, `enabled`, `known`, parameter map, `opaque_payload` | Typed/versioned processing node. Unknown future effects remain opaque and disabled. |
| `Transform` | position, scale, rotation, anchor, crop edges, opacity | Visual placement and crop state in sequence space. |

## Media and timeline entities

| Type | Significant fields | Description |
| --- | --- | --- |
| `Asset` | identity/name/URI/fingerprint, duration, stream flags and descriptors, metadata | Reference to original media and its probed descriptors. Media bytes are not embedded. |
| `Clip` | identity, asset/kind/name, timeline/source ranges, rate/reverse/link, transform/blend/audio/effects/title | One non-destructive timeline use of media or generated title content. |
| `Gap` | `timeline_range` | Derived half-open empty range. It has no persistent identity. |
| `Track` | identity/kind/name, lock/mute/solo, `visible`, `targeted`, clips/effects, audio gain/pan | Ordered timeline lane. Visibility controls visual composition; targeting is an editorial routing hint. Audio tracks add canonical mixer state. |
| `Marker` | identity, exact range, label, color | Sequence annotation. A zero-duration range is a point marker. |
| `CaptionStyle` | font, size, text/background colors, bold/italic, alignment, normalized vertical position/safe margin, outline | Persistent renderer-actionable caption styling. |
| `CaptionWord` | identity, text, exact half-open range, probability | Stable word-level timing contained by one cue. |
| `CaptionProvenance` | word source, model identity | Origin of timed words without repeating model data per token. |
| `Caption` | identity, exact range, text, language, style, ordered words, provenance | Sequence-owned caption cue. |
| `Transition` | identity, outgoing/incoming clip IDs, exact range, kind, enabled | Sequence-owned relation over one adjacent video-track cut. |
| `Sequence` | identity/name, frame rate/raster/audio format, ordered tracks, markers, captions, transitions | Complete editorial timeline. |
| `Project` | identity/name, assets, sequences, metadata | Root authoritative state serialized into project snapshots. |

## Validation relationships

A video track accepts video and title clips; an audio track accepts audio clips. Title clips use a
nil asset ID and require a `Title`; media clips require an existing compatible asset and must not
carry title state. Clip ranges are positive, non-overlapping within a track, and source-bounded.
Locked tracks reject structural edits. Track order, name, visibility, targeting, mute/solo,
−96…+24 dB gain, −1…+1 pan, and track effects are project state and therefore round-trip through
checkpoints. Known clip/track effects and their keyframes are validated before a revision is
published; unknown future effects remain opaque and disabled.

An enabled transition refers to adjacent clips on the same video track. Its range begins inside the
outgoing clip, ends inside the incoming clip, strictly straddles their shared cut, and requires
available source handles. Enabled transition ranges on one track cannot overlap.

Caption words are nonempty, finite-probability, chronological, non-overlapping, and contained by the
cue's half-open range. Style colors/channels and numeric layout fields are finite and bounded.
Unknown or older snapshot caption state receives canonical defaults at the codec boundary rather
than weakening model validation.

## Lookup and duration functions

### `const Asset* findAsset(const Project& project, EntityId id) noexcept`

Returns a borrowed pointer to the matching asset or null.

### `const Sequence* findSequence(const Project& project, EntityId id) noexcept`

Returns a borrowed pointer to the matching sequence or null.

### `const Track* findTrack(const Sequence& sequence, EntityId id) noexcept`

Returns a borrowed pointer to the matching track or null.

### `const Clip* findClip(const Sequence& sequence, EntityId id) noexcept`

Searches all sequence tracks and returns a borrowed pointer to the matching clip or null.

### `const Transition* findTransition(const Sequence& sequence, EntityId id) noexcept`

Returns a borrowed pointer to the matching sequence-owned transition or null.

### `Time sequenceDuration(const Sequence& sequence)`

Returns the latest end among clips, captions, markers, and transitions using exact time arithmetic.
The function may report arithmetic failure through the model's normal exception policy.

Borrowed pointers above must not outlive or be used after mutation of the owning value. Pointers
from an immutable snapshot remain valid for that snapshot's lifetime.

## Usage example

```cpp
edit::Track voice;
voice.kind = edit::TrackKind::Audio;
voice.name = "A1 Dialogue";
voice.visible = true;
voice.targeted = true;

edit::Sequence sequence;
sequence.name = "Main";
sequence.frame_rate = edit::Rate{30'000, 1'001};
sequence.tracks.push_back(std::move(voice));
```

AI assistance has been used to create this output.

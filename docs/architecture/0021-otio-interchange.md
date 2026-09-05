<!-- SPDX-License-Identifier: MPL-2.0 -->

# OTIO JSON interchange (P1)

First-party OpenTimelineIO JSON import/export lives in `src/interchange` as static library
`video_editor_interchange`. The module uses Qt 6 Core `QJsonDocument` for parsing and
serialization; no additional JSON dependency is required.

## Scope

| Edit model | OTIO schema | Notes |
| --- | --- | --- |
| `Sequence` | `Timeline.1` | Name, frame rate, width/height in `metadata.video_editor` |
| `Track` | `Track.1` under `Stack.1` | `kind`: Video / Audio |
| `Clip` (media) | `Clip.1` + `ExternalReference.1` | `target_url` ↔ `Asset.source_uri` |
| Derived gap | `Gap.1` | Exported between non-overlapping clips; import advances playhead only |
| `Marker` | `Marker.1` | Label, range, RGBA in `metadata.video_editor` |
| `ClipKind::NestedSequence` | `Stack.1` track child | Inline nested tracks; import creates a new `Sequence` + nested clip |
| Effects / LUT / mixer DSP / captions | `metadata.video_editor` on export | Import skips unknown vendor plugins with `OtioReport.skipped`; does not fail the file |

## Time conversion

OTIO `RationalTime.1` uses `{ value, rate }` where `rate` is a floating-point rate in Hz or fps
(for example `30.0`, `48000.0`). Our `Time` stores exact rational `{ value, timescale }`.

Conversion uses the sequence frame rate (video) or `48000` (audio) as the target timescale:

- **Export:** `otio_value = round(seconds × rate)` where `seconds = time.value / time.timescale`
- **Import:** `time.value = round(otio_value × timescale / otio_rate)`

Integer-friendly rates (24, 25, 30, 48000) are preferred for interoperability.

## Fail-closed rules

Import fails when a composable would drop media identity:

- `Clip.1` without `ExternalReference.1` and without a nested `Stack.1` reference

Unknown vendor effect/filter plugins (`*.Effect.1`, `*.Filter.1`) are skipped and recorded in
`OtioReport.skipped`.

## Assets

Import creates `Asset` rows from `ExternalReference.target_url`. Probing is not performed in the
interchange module; `has_video` / `has_audio` are inferred from track kind and `fingerprint` may
remain empty until the app relinks or probes.

## API

```cpp
struct OtioReport { std::vector<std::string> warnings; std::vector<std::string> skipped; };

edit::Result<std::string, std::string> export_otio_json(
    const edit::Project& project, edit::EntityId sequence_id, OtioReport* report = nullptr);

edit::Result<edit::Project, std::string> import_otio_json(
    std::string_view json, OtioReport* report = nullptr);
```

## Tests

`tests/interchange/otio_test.cpp` covers round-trip (two tracks, nested stack, markers, URIs),
unknown effect skip+report, and fail-closed missing media identity.

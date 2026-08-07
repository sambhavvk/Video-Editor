<!-- SPDX-License-Identifier: MPL-2.0 -->

# Caption service

`VideoEditor::CaptionService` is the dependency-light subtitle boundary for the
editor. It parses and writes UTF-8 SRT and WebVTT, keeps timing as exact
`video_editor::edit::Time`, validates authoring order and overlaps, reflows text
without splitting words, searches transcript text, and converts cues to the edit
model's `Caption` type.

The parser normalizes BOM and line endings while retaining cue identifiers,
multiline text, WebVTT cue settings, and header metadata. Serialization is
deterministic. Its default timestamp policy rejects a time that cannot be
represented exactly in milliseconds; callers must explicitly opt into nearest
millisecond rounding when exporting frame-based caption timing.

The stable public include is:

```cpp
#include <video_editor/caption_service/caption_service.h>
```

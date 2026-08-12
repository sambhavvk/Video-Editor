<!-- SPDX-License-Identifier: MPL-2.0 -->

# `CpuRenderer` API reference

Header: `video_editor/render_engine/cpu_renderer.h`

Namespace: `video_editor::render`

`CpuRenderer` is the deterministic correctness path for preview and export. Construction takes
shared ownership of a `FrameProvider`; a null provider is invalid. The provider supplies decoded
media frames in-process. Title clips are generated from model state and do not call it.

`begin_epoch` atomically publishes the newest request epoch. `current_epoch` returns it.
`request_frame` renders one exact timeline time from an immutable `TimelineSnapshot`, a
`PreviewProfile`, and the caller's epoch. A stale epoch fails before decode or before publication if
it changes during provider work.

The result is a `VideoFrame` backed by an owned immutable `CpuFrame`. Output is linear, full-range,
premultiplied RGBA float32 on an opaque-black Rec.709 SDR reference canvas. Half and quarter preview
profiles change output dimensions while preserving sequence-space transform meaning.

Video tracks with `visible == false` contribute no video/title layers. Targeting is an editorial
routing hint and has no render effect. This rule is shared with the GPU path so preview fallback and
reference export retain the same track-output result.

Canonical title rendering uses a bundled deterministic glyph raster, including a replacement glyph
for unsupported text. Font size, alignment, bold, italic, foreground, and background are evaluated
without platform font dependencies. Active transitions evaluate in their exact half-open ranges;
cross-dissolve and dip-to-black request both adjacent sources with exact source-handle mapping.

`CpuRenderer` is safe for concurrent request calls only when its shared `FrameProvider` supports
that use. Epoch access itself is atomic. Snapshot data is borrowed for the duration of a call; frame
results own their storage.

AI assistance has been used to create this output.

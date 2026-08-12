# Export service

`VideoEditor::ExportService` is the deterministic CPU reference A/V export path.
It consumes one immutable `TimelineSnapshot`, a shared `CpuRenderer`, and—when
audio is requested—a shared `audio_render::TimelineAudioRenderer`. Video always
uses full-resolution frames with proxies disabled and expensive effects enabled.
The audio renderer's provider contract can resolve only authoritative originals;
it has no proxy field.

Two creator-safe intermediate presets are exposed:

- lossless 10-bit FFV1 in Matroska;
- 10-bit ProRes 422 HQ in MOV when a compatible encoder is present.

Frames are sampled at exact rational sequence-frame times over the half-open
timeline duration. Linear float RGBA is transformed to limited-range Rec.709 in
a deterministic integer packing path. Optional audio is pulled in consecutive,
exact half-open blocks from sample zero through
`ceil(sequence_duration * 48000)`, quantized deterministically, and muxed as
48 kHz stereo PCM signed 16-bit little-endian in both presets. Twenty-millisecond
audio packets preserve exact starts with Matroska's millisecond time scale while
keeping mux interleave bounded. A single encoder thread plus FFmpeg's bit-exact
flags make repeated FFV1/Matroska A/V exports byte stable on the supported
runtime.

`include_audio=false` preserves the original video-only behavior. When it is
true, a timeline audio renderer is required. `ExportResult` reports the exact
audio sample count, encoded duration, and codec. Typed audio-render failures are
retained in `ExportError::audio_render_error`; renderer cancellation maps to the
top-level `Cancelled` code.

The encoder writes only to a unique sibling temporary file. Cancellation and
all pre-commit errors remove that file. The finished file is flushed and moved
to the requested destination with an atomic replace/no-replace operation on
Windows and Linux, so an existing destination is never partially overwritten.

Tests decode both output streams and cover exact video frames, audio samples and
timestamps, byte determinism, both container presets, missing-renderer
validation, typed original-media errors, and cancellation/overwrite safety.

# Export service

`VideoEditor::ExportService` is the deterministic CPU reference video-export
path. It consumes an immutable `TimelineSnapshot` and a shared `CpuRenderer`,
always requesting full-resolution frames with proxies disabled and expensive
effects enabled.

The current contract is deliberately video-only. `ExportRequest::include_audio`
must remain false, and `ExportResult::audio_exported` is always false. This keeps
the beta implementation honest until the 48 kHz master graph can be encoded and
muxed through the same revision-bound job.

Two creator-safe intermediate presets are exposed:

- lossless 10-bit FFV1 in Matroska;
- 10-bit ProRes 422 HQ in MOV when a compatible encoder is present.

Frames are sampled at exact rational sequence-frame times over the half-open
timeline duration. Linear float RGBA is transformed to limited-range Rec.709 in
a deterministic integer packing path. A single encoder thread plus FFmpeg's
bit-exact flags make repeated FFV1/Matroska exports byte stable on the supported
runtime.

The encoder writes only to a unique sibling temporary file. Cancellation and
all pre-commit errors remove that file. The finished file is flushed and moved
to the requested destination with an atomic replace/no-replace operation on
Windows and Linux, so an existing destination is never partially overwritten.

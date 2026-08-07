# CPU playback provider

`video_editor_playback` owns persistent FFmpeg demuxer/decoder sessions and adapts them to the
render engine's `FrameProvider` contract. Requests are in exact, source-relative `edit::Time`.
Backward or distant requests seek to the preceding keyframe, flush the decoder, and decode forward
in presentation order. Nearby forward requests continue from the existing decoder state.

The current CPU conversion path uses libswscale with Rec.709 YUV/RGB coefficients and then applies
the inverse Rec.709 transfer curve to 16-bit RGBA samples. This is intentionally a documented
Rec.709 approximation: it does not yet perform chromatic adaptation, HDR tone mapping, or a
fully color-managed source-to-working-space transform. The resulting `CpuFrame` is float RGBA with
straight alpha and approximately scene-linear Rec.709 RGB values.

Each provider has an explicit request epoch. Changing the epoch interrupts FFmpeg I/O, and stale
work returns `RenderErrorCode::StaleRequest`. A session interrupted by cancellation or affected by
a demux/decode/conversion failure is discarded. Non-cancellation failures are retried once with a
freshly opened session so a poisoned decoder is never reused.

<!-- SPDX-License-Identifier: MPL-2.0 -->

# `GpuTimelineRenderer` API reference

Header: `video_editor/render_engine/gpu_timeline_renderer.h`

Namespace: `video_editor::render`

`GpuTimelineRenderer` takes shared ownership of a `FrameProvider` and `GpuRenderer`. It maps active
media clips to GPU layers without first rendering a CPU composite. The GPU object owns its
libplacebo device and backend resources.

`begin_epoch`, `current_epoch`, and `request_frame` follow the same cancellation contract as
`CpuRenderer`. A successful request returns an owned `GpuImage` that shares the device lifetime.

The native timeline path supports the explicitly documented transform and normal-composition
subset. An active title or enabled transition returns `RenderErrorCode::GpuUnsupportedTimeline`.
This is a capability signal, not a device failure: the controller must render that frame through
`CpuRenderer`, keep the GPU session available, and retry native GPU rendering on a later supported
frame. Device-loss errors are separate and may latch session fallback.

Invisible video tracks are skipped before active-layer capability checks and composition. Track
targeting does not affect output. The CPU fallback applies the same visibility rule.

The renderer, provider, and backend establish their own synchronization requirements. The epoch is
atomic, but callers must not assume concurrent GPU command submission is safe unless the concrete
backend contract says so.

AI assistance has been used to create this output.

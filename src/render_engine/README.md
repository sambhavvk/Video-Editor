<!-- SPDX-License-Identifier: MPL-2.0 -->

# Render engine

`video_editor_render_engine` is the deterministic CPU reference compositor for
the current vertical slice. Consumers link `video_editor::render_engine` and
provide a `FrameProvider`; raw frames remain in process.

`CpuRenderer::request_frame` consumes an immutable `TimelineSnapshot`, exact
timeline `Time`, a `PreviewProfile`, and a request epoch. A mismatched epoch is
rejected before decoding, and an epoch superseded during provider work is
discarded before presentation.

## Transform contract

The renderer honors every field of `edit::Transform`:

- The default transform centers an unscaled source in the sequence canvas.
- `position` is an offset in full-resolution sequence pixels. Half- and
  quarter-resolution previews scale the offset with the canvas.
- `anchor_x` and `anchor_y` are normalized source coordinates. That pivot is
  placed at canvas center plus `position`, and scale and rotation happen around
  it.
- Signed `scale` values flip the corresponding axis. Positive rotation is
  clockwise in screen coordinates.
- Crop values are normalized source-edge fractions and mask source taps before
  interpolation.
- `opacity` multiplies premultiplied source color and alpha during compositing.

Destination pixel centers are inverse-mapped into source pixel-center space.
The reference sampler is deterministic bilinear interpolation. Samples beyond
the source extent are transparent rather than edge-clamped; crop-excluded taps
are also transparent. The CPU implementation intentionally scans the full
destination and favors clarity and golden-test stability over throughput.

## Alpha and blend modes

`CpuFrame` stores linear premultiplied RGBA float32. Blend functions operate on
unpremultiplied, clamped SDR color and are combined using the premultiplied
source-over equation. `Normal`, `Add`, `Multiply`, `Screen`, and `Overlay` are
implemented. Output channels and alpha are clamped to `[0, 1]` for the Rec.709
SDR reference path.

The preview currently begins with an opaque black canvas. Consequently,
transparent transformed or cropped regions reveal lower tracks, or black when
there is no lower image. Tracks are composited in sequence order from bottom to
top; muted and non-video tracks are skipped.

## Focused development build

The module can be configured independently of Qt and FFmpeg:

```sh
cmake -S src/render_engine -B build/render-engine -G Ninja \
  -DVIDEO_EDITOR_RENDER_ENGINE_BUILD_TESTS=ON
cmake --build build/render-engine
ctest --test-dir build/render-engine --output-on-failure
```

The CPU path remains the correctness oracle and mandatory fallback for the
libplacebo backend described below.

## Titles and transitions

`CpuRenderer` is also the reference implementation for canonical title and
transition output:

- title clips rasterize directly from edit-model title payloads without asking
  the media provider to decode anything;
- unsupported glyphs render with one deterministic replacement glyph, so the
  same title payload produces the same golden image on every machine;
- enabled `CrossDissolve` and `DipToBlack` transitions evaluate in exact
  half-open timeline ranges and use source-handle extrapolation from the
  adjacent outgoing/incoming clips.

This keeps preview and export behavior aligned with the same immutable snapshot
contract used by the rest of the CPU compositor.

## Capability-gated GPU foundation

`gpu_backend.h` provides a typed API independent of Qt:

- `gpu_build_capabilities()` distinguishes what was compiled from what a
  runtime device can actually initialize. Linux builds expose Vulkan only;
  Windows builds expose D3D11 only.
- `GpuRenderer::create()` always returns an inspectable object. Callers must
  require `capabilities().available()` and `offscreen_rendering`; a build stub,
  unsupported selection, initialization failure, or lost device never reports
  itself as ready.
- `upload()` accepts only CPU-backed, linear full-range Rec.709,
  premultiplied RGBA float32 `VideoFrame` values. Native GPU-frame import is
  not enabled until API-specific synchronization ownership is implemented.
- `GpuImage` owns its libplacebo texture and shares the device lifetime. It may
  outlive `GpuRenderer`; its texture and device are released after the last
  image reference is destroyed.
- `composite()` scales layers and composites them bottom-to-top using
  premultiplied source-over into an RGBA float32 GPU target. `download()` is a
  blocking reference readback for parity tests, export, and CPU fallback.
- `GpuTimelineRenderer` walks an immutable snapshot without first invoking the
  CPU compositor. It decodes only active clips, uploads each clip separately,
  and uses `composite_timeline()` for crop, position, signed/non-uniform scale,
  normalized anchor, rotation (including a moved pivot), opacity, titles,
  transitions, and the five premultiplied blend modes. Muted video tracks are skipped, source-time mapping
  remains exact rational time, and request epochs reject stale decode work.
- Timeline output begins as opaque black, matching the CPU reference even when
  no clips are active. Unknown enabled clip effect types
  return `GpuUnsupportedTimeline`; callers must use the CPU path for that frame
  rather than display an approximation.
- `present()` renders to the swapchain configured at creation. An inaccessible
  surface is a recoverable presentation error. A libplacebo-reported failed GPU
  latches `DeviceLost`, as does `notify_device_lost()` when the platform owner
  independently detects removal or reset. Every later operation then returns
  `GpuDeviceLost` so the session can stay on `CpuRenderer` until the backend is
  explicitly recreated.

`NativePresentationSurface` contains borrowed handles. On Linux, `instance`
is the `VkInstance` that owns the supplied `VkSurfaceKHR`; both must outlive the
renderer. Vulkan 1.2 or newer is required by libplacebo. On Windows, `surface`
is an `HWND`, `instance` is ignored, and a D3D feature level of at least 11.0
is requested. An empty surface creates an offscreen-only renderer. The API
does not create a hidden window or silently substitute presentation support.

The CMake option `VIDEO_EDITOR_ENABLE_GPU_RENDERING` defaults to `ON`. The
backend links only the exact `VIDEO_EDITOR_LIBPLACEBO_VERSION`. On Linux,
Vulkan headers are also required. Missing or incompatible dependencies compile
the same public API as a truthful unavailable stub, leaving CPU-only builds and
the `core-only` preset functional.

The current GPU path accelerates timeline transform/crop/opacity and normal
source-over composition, color-aware libplacebo rendering, readback, and
swapchain presentation. Decode still produces CPU RGBA frames that are
uploaded per active clip. Non-normal blend shaders, titles, effects,
transitions, YUV/native-handle uploads, zero-copy decode, shader/texture
pooling, and UI-owned swapchain creation remain later integration work. The CPU
renderer is the required typed fallback for all unsupported or failed GPU
frames.

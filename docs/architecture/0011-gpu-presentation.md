<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0011: Capability-gated libplacebo GPU presentation

- **Status:** accepted
- **Date:** 2026-08-09
- **Owners:** Core/Media, Desktop/Product, and Quality/Platform

## Context

The CPU compositor is the deterministic correctness oracle but cannot meet the target 4K30 preview
budget alone. Windows and Linux also expose different native presentation APIs. A build that merely
finds libplacebo must not imply that a usable hardware device, presentation surface, or validated
driver exists. GPU failure must remain recoverable because project state and export correctness do
not depend on the GPU.

## Decision

- `VIDEO_EDITOR_ENABLE_GPU_RENDERING` defaults to `ON`. CMake accepts only the pinned libplacebo
  contract. If it is disabled, unavailable, the wrong version, or the host is unsupported, the
  render target still compiles a truthful stub and the mandatory CPU renderer remains available.
- Windows supports the D3D11 backend; Linux supports Vulkan. `Auto` selects the platform-native
  compiled backend. Requesting a backend that was not compiled or is not native fails closed with a
  diagnostic rather than silently selecting another API.
- Build support and runtime support are different contracts. `GpuBuildCapabilities` reports the
  compiled libplacebo/version and backend set. `GpuCapabilities` reports the selected backend,
  unavailable/ready/device-lost state, offscreen and presentation support, whether a software device
  was admitted, API version, and a local diagnostic.
- Hardware is required by default. Software Vulkan/D3D implementations are considered only when
  `GpuOptions::allow_software` is explicitly true. Debug layers are likewise opt-in.
- An optional typed `NativePresentationSurface` carries an `HWND` on Windows or a caller-owned
  `VkInstance` plus `VkSurfaceKHR` on Linux. A zero surface requests offscreen operation. Handle and
  synchronization ownership remain explicit; this slice does not import arbitrary native decoder
  textures.
- Upload accepts the existing CPU `VideoFrame` contract: linear full-range Rec.709 premultiplied
  RGBA float32. GPU images retain lifetime-owned device resources. The GPU timeline path decodes
  only active video clips and composes crop, position, non-uniform scale, a custom anchor or
  centered-pivot rotation, opacity, and Normal premultiplied source-over bottom-to-top. It does not
  yet combine rotation with a moved pivot. Enabled effects, title clips, and other blend modes return
  an explicit unsupported-timeline error so the caller can fall back without changing semantics.
  Blocking download exists for parity testing, export/reference work, and controlled fallback.
- Presentation is explicit. A temporarily inaccessible surface returns a recoverable error; a
  failed device changes capabilities to `DeviceLost`. Platform code may forward an independently
  observed loss through `notify_device_lost`. Later operations then fail deterministically.
- The owning scheduler/controller—not the backend—selects CPU fallback. It records the diagnostic,
  invalidates GPU-only resources, advances the request epoch, and requests the current immutable
  revision again through `CpuRenderer`. No editorial state changes during fallback.
- The desktop permits only one preview request in flight. Continuous transport ticks update the
  desired playhead and coalesce behind that request instead of invalidating it every 16 ms. A
  completed frame may be presented while the newest desired position is queued, after which the
  scheduler immediately renders the latest position. Explicit seeks, edits, and project changes
  still advance the epoch and cancel obsolete decode work.
- The Qt controller requests the per-clip GPU timeline renderer before `CpuRenderer`, downloads the
  offscreen result for `QImage`, and exposes an active-GPU diagnostic for tests/UI. An unsupported
  timeline feature (effects, title, or non-Normal blend) falls back to CPU for that frame without
  disabling later GPU attempts. Backend/device/upload/render/readback failures preserve a CPU frame
  for the request and latch CPU fallback for the remainder of the session. No desktop path may claim
  native presentation until it owns an actual swapchain/surface.

## Consequences

- CPU-only developer and recovery builds remain buildable and honest even with the feature option
  enabled.
- Capability UI and diagnostics can distinguish not compiled, no suitable device, offscreen-only,
  ready presentation, admitted software device, and device loss.
- The initial upload path performs a host copy. D3D11VA and VAAPI/DMA-BUF zero-copy remain blocked on
  explicit synchronization and hardware-matrix validation.
- GPU output must match CPU transform, crop, alpha, blend, track-order, and out-of-bounds semantics
  within declared tolerances; implementation convenience cannot change edit semantics.
- D3D11 on Windows and Vulkan on Linux are platform requirements for acceleration, not requirements
  to open, edit, recover, or CPU-export a project.
- The desktop currently creates the libplacebo device on a worker thread after the
  controller is constructed. CPU preview is available immediately; GPU attaches when
  the device is ready. A slow or unhealthy graphics driver no longer delays the
  Qt startup thread. Public-beta hardening still needs a bounded wait diagnostic and
  native swapchain presentation.

## Verification

All builds test pure backend selection and truthful stub diagnostics without requiring a GPU.
Feature-enabled jobs test create/upload/composite/download and device-loss state when the pinned
library and a permitted device exist. Presentation and loss recovery require platform-native tests.
Release evidence must include CPU/GPU golden-frame comparisons and Intel/AMD/NVIDIA coverage on
Windows D3D11 plus Linux Vulkan under Wayland and X11.

Focused Linux verification covers a real Vulkan create/upload/composite/download CPU-parity pass,
per-clip transform/composition behavior, strict-warning and sanitizer builds, the dependency-disabled
truthful stub, a desktop active-GPU smoke test, and continuous presentation when a frame takes longer
than the transport timer interval. Platform GUI parity still needs broader tests.
The desktop preserves the CPU result on failure. Native Windows/Linux presentation, the completed
codec/GPU matrix, asynchronous device startup, and long-session device-loss tests remain public-beta
gates.

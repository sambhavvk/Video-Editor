// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/frame.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace video_editor::render {

enum class GpuBackendKind : std::uint8_t { Auto, D3D11, Vulkan };
enum class GpuRuntimeState : std::uint8_t { Unavailable, Ready, DeviceLost };

struct GpuBuildCapabilities {
  bool libplacebo{false};
  bool d3d11{false};
  bool vulkan{false};
  GpuBackendKind native_default{GpuBackendKind::Auto};
  std::string libplacebo_version;
};

struct GpuBackendSelection {
  bool supported{false};
  GpuBackendKind backend{GpuBackendKind::Auto};
  std::string diagnostic;
};

// Pure selection policy, exposed for deterministic platform-matrix tests.
// Auto uses native_default when available, then the only compiled backend.
[[nodiscard]] GpuBackendSelection select_gpu_backend(const GpuBuildCapabilities& build,
                                                     GpuBackendKind requested);

[[nodiscard]] GpuBuildCapabilities gpu_build_capabilities();

// Opaque platform presentation handles. On Linux, instance is a VkInstance
// and surface is a VkSurfaceKHR created from that instance. On Windows,
// instance is ignored and surface is an HWND. Zero surface means offscreen.
struct NativePresentationSurface {
  GpuBackendKind backend{GpuBackendKind::Auto};
  std::uintptr_t instance{0};
  std::uintptr_t surface{0};
  int width{0};
  int height{0};
};

struct GpuOptions {
  GpuBackendKind preferred_backend{GpuBackendKind::Auto};
  bool allow_software{false};
  bool enable_debug_layers{false};
  NativePresentationSurface presentation{};
};

struct GpuCapabilities {
  GpuBackendKind backend{GpuBackendKind::Auto};
  GpuRuntimeState state{GpuRuntimeState::Unavailable};
  bool offscreen_rendering{false};
  bool presentation{false};
  // Known on D3D11. Vulkan is false when software adapters were forbidden,
  // and unknown when allow_software permitted either class of device.
  std::optional<bool> software_device;
  std::string api_version;
  std::string diagnostic;

  [[nodiscard]] bool available() const noexcept {
    return state == GpuRuntimeState::Ready;
  }
};

class GpuImage final {
public:
  GpuImage() = default;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] edit::Time timestamp() const noexcept;
  [[nodiscard]] edit::Time duration() const noexcept;
  [[nodiscard]] const FrameColor& color() const noexcept;
  [[nodiscard]] AlphaMode alpha_mode() const noexcept;

private:
  struct Storage;
  explicit GpuImage(std::shared_ptr<Storage> storage);

  std::shared_ptr<Storage> storage_;
  friend class GpuRenderer;
};

// A decoded clip image plus the non-destructive timeline properties that the
// GPU compositor applies. Coordinates retain edit-model semantics: position
// is expressed in full-resolution sequence pixels, anchor and crop are
// normalized to the decoded source, and positive rotation is clockwise.
struct GpuLayer final {
  GpuImage image;
  edit::Transform transform{};
  edit::BlendMode blend_mode{edit::BlendMode::Normal};
};

class GpuRenderer final {
public:
  [[nodiscard]] static std::unique_ptr<GpuRenderer> create(const GpuOptions& options = {});

  ~GpuRenderer();
  GpuRenderer(const GpuRenderer&) = delete;
  GpuRenderer& operator=(const GpuRenderer&) = delete;
  GpuRenderer(GpuRenderer&&) noexcept;
  GpuRenderer& operator=(GpuRenderer&&) noexcept;

  [[nodiscard]] GpuCapabilities capabilities() const;

  // Accepts the current CPU VideoFrame contract: linear, full-range Rec.709,
  // premultiplied RGBA float32. NativeGpu import is intentionally not yet
  // enabled because it requires API-specific synchronization ownership.
  [[nodiscard]] RenderResult<GpuImage> upload(const VideoFrame& frame);

  // Composites same-device layers bottom-to-top with premultiplied source-over
  // and bilinear scaling into a lifetime-owned GPU texture.
  [[nodiscard]] RenderResult<GpuImage> composite(std::span<const GpuImage> layers, int width,
                                                 int height, edit::Time timestamp,
                                                 edit::Time duration);

  // Timeline compositor used by preview rendering. It applies crop,
  // position, non-uniform scale, anchor, center-pivot rotation and opacity on
  // the GPU. The target starts as opaque black to match CpuRenderer. Only
  // premultiplied source-over is currently supported; other blend modes
  // return GpuUnsupportedTimeline so callers can fall back without rendering
  // a visually incorrect frame. Rotation combined with a moved position or
  // non-default anchor also uses that typed fallback until its sampling parity
  // is proven. Empty layers produce an opaque-black frame.
  [[nodiscard]] RenderResult<GpuImage>
  composite_timeline(std::span<const GpuLayer> layers, int width, int height,
                     std::uint32_t sequence_width, std::uint32_t sequence_height,
                     edit::Time timestamp, edit::Time duration);

  // Blocking reference readback for export, parity testing, and fallback.
  [[nodiscard]] RenderResult<VideoFrame> download(const GpuImage& image);

  // Presents to the surface supplied at creation. Returns a recoverable error
  // when a surface is temporarily inaccessible, and DeviceLost only when
  // libplacebo reports a failed GPU.
  [[nodiscard]] RenderResult<bool> present(const GpuImage& image);

  // Allows an owning platform integration to forward an independently
  // observed D3D/Vulkan device-loss notification. All later GPU operations
  // fail deterministically so the caller can switch to CpuRenderer.
  void notify_device_lost(std::string diagnostic);

private:
  struct Impl;
  explicit GpuRenderer(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;
};

} // namespace video_editor::render

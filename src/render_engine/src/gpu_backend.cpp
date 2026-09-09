// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/gpu_backend.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <numbers>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#if VIDEO_EDITOR_HAS_LIBPLACEBO
#include <libplacebo/config.h>
#include <libplacebo/filters.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/swapchain.h>
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
#include <libplacebo/d3d11.h>
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
#include <libplacebo/vulkan.h>
#endif
#endif

namespace video_editor::render {
namespace {

[[nodiscard]] bool supports(const GpuBuildCapabilities& build, const GpuBackendKind backend) {
  switch (backend) {
  case GpuBackendKind::D3D11:
    return build.d3d11;
  case GpuBackendKind::Vulkan:
    return build.vulkan;
  case GpuBackendKind::Auto:
    return false;
  }
  return false;
}

[[nodiscard]] RenderError unavailable_error(const GpuCapabilities& capabilities) {
  const RenderErrorCode code = capabilities.state == GpuRuntimeState::DeviceLost
                                   ? RenderErrorCode::GpuDeviceLost
                                   : RenderErrorCode::GpuUnavailable;
  return {.code = code,
          .message = capabilities.diagnostic.empty() ? "GPU rendering is unavailable"
                                                     : capabilities.diagnostic};
}

} // namespace

GpuBackendSelection select_gpu_backend(const GpuBuildCapabilities& build,
                                       const GpuBackendKind requested) {
  if (!build.libplacebo) {
    return {.supported = false,
            .backend = requested,
            .diagnostic = "libplacebo was not linked into this build"};
  }
  if (requested != GpuBackendKind::Auto) {
    if (supports(build, requested)) {
      return {.supported = true, .backend = requested, .diagnostic = {}};
    }
    return {.supported = false,
            .backend = requested,
            .diagnostic = requested == GpuBackendKind::D3D11
                              ? "the build does not contain the D3D11 backend"
                              : "the build does not contain the Vulkan backend"};
  }
  if (supports(build, build.native_default)) {
    return {.supported = true, .backend = build.native_default, .diagnostic = {}};
  }
  if (build.d3d11 != build.vulkan) {
    return {.supported = true,
            .backend = build.d3d11 ? GpuBackendKind::D3D11 : GpuBackendKind::Vulkan,
            .diagnostic = {}};
  }
  return {.supported = false,
          .backend = GpuBackendKind::Auto,
          .diagnostic = build.d3d11 && build.vulkan
                            ? "automatic backend selection has no native platform preference"
                            : "this build contains no supported GPU backend"};
}

GpuBuildCapabilities gpu_build_capabilities() {
  GpuBuildCapabilities result;
#if VIDEO_EDITOR_HAS_LIBPLACEBO
  result.libplacebo = true;
  result.libplacebo_version = pl_version();
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
  result.d3d11 = true;
  result.native_default = GpuBackendKind::D3D11;
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
  result.vulkan = true;
  result.native_default = GpuBackendKind::Vulkan;
#endif
#endif
  return result;
}

#if VIDEO_EDITOR_HAS_LIBPLACEBO && ((defined(_WIN32) && defined(PL_HAVE_D3D11)) ||                 \
                                    (defined(__linux__) && defined(PL_HAVE_VULKAN)))

namespace {

namespace {

constexpr int kCurveLutSamples = 256;

struct LutTextureCacheEntry final {
  int lattice_size{0};
  pl_tex texture{nullptr};
};

struct CurvesTextureCacheEntry final {
  pl_tex texture{nullptr};
};

} // namespace

struct LibplaceboState final {
  mutable std::mutex mutex;
  GpuCapabilities capabilities;
  pl_log log{nullptr};
  pl_gpu gpu{nullptr};
  pl_renderer renderer{nullptr};
  pl_swapchain swapchain{nullptr};
  pl_swapchain secondary_swapchain{nullptr};
  bool last_secondary_present_succeeded{false};
  std::unordered_map<std::string, LutTextureCacheEntry> lut_texture_cache;
  std::unordered_map<std::string, CurvesTextureCacheEntry> curves_texture_cache;
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
  pl_d3d11 d3d11{nullptr};
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
  pl_vulkan vulkan{nullptr};
#endif

  ~LibplaceboState() {
    std::scoped_lock lock(mutex);
    for (auto& [key, entry] : lut_texture_cache) {
      if (entry.texture != nullptr) {
        pl_tex_destroy(gpu, &entry.texture);
      }
    }
    for (auto& [key, entry] : curves_texture_cache) {
      if (entry.texture != nullptr) {
        pl_tex_destroy(gpu, &entry.texture);
      }
    }
    if (secondary_swapchain != nullptr) {
      pl_swapchain_destroy(&secondary_swapchain);
    }
    if (swapchain != nullptr) {
      pl_swapchain_destroy(&swapchain);
    }
    if (renderer != nullptr) {
      pl_renderer_destroy(&renderer);
    }
    if (gpu != nullptr) {
      pl_gpu_finish(gpu);
    }
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
    if (d3d11 != nullptr) {
      pl_d3d11_destroy(&d3d11);
    }
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
    if (vulkan != nullptr) {
      pl_vulkan_destroy(&vulkan);
    }
#endif
    gpu = nullptr;
    if (log != nullptr) {
      pl_log_destroy(&log);
    }
  }
};

void mark_device_lost(LibplaceboState& state, std::string message) {
  state.capabilities.state = GpuRuntimeState::DeviceLost;
  state.capabilities.offscreen_rendering = false;
  state.capabilities.presentation = false;
  state.capabilities.secondary_presentation = false;
  state.capabilities.diagnostic = std::move(message);
}

[[nodiscard]] bool detect_device_loss(LibplaceboState& state, const std::string& operation) {
  if (state.gpu != nullptr && pl_gpu_is_failed(state.gpu)) {
    mark_device_lost(state, "GPU device was lost while " + operation +
                                "; switch to the CPU renderer and recreate the GPU backend");
    return true;
  }
  return false;
}

[[nodiscard]] pl_frame make_frame(const pl_tex texture, const int width, const int height) {
  pl_frame frame{};
  frame.num_planes = 1;
  frame.planes[0].texture = texture;
  frame.planes[0].components = 4;
  frame.planes[0].component_mapping[0] = 0;
  frame.planes[0].component_mapping[1] = 1;
  frame.planes[0].component_mapping[2] = 2;
  frame.planes[0].component_mapping[3] = 3;
  frame.crop = {
      .x0 = 0.0F, .y0 = 0.0F, .x1 = static_cast<float>(width), .y1 = static_cast<float>(height)};
  frame.repr = pl_color_repr_rgb;
  frame.repr.levels = PL_COLOR_LEVELS_FULL;
  frame.repr.alpha = PL_ALPHA_PREMULTIPLIED;
  frame.color.primaries = PL_COLOR_PRIM_BT_709;
  frame.color.transfer = PL_COLOR_TRC_LINEAR;
  return frame;
}

[[nodiscard]] bool valid_cpu_frame(const VideoFrame& frame, std::string& diagnostic) {
  if (frame.width <= 0 || frame.height <= 0) {
    diagnostic = "GPU upload requires positive frame dimensions";
    return false;
  }
  if (frame.layout != PixelLayout::RgbaFloat32 || frame.bit_depth != 32 ||
      !std::holds_alternative<std::shared_ptr<const CpuFrame>>(frame.storage)) {
    diagnostic = "GPU upload currently requires CPU-backed RGBA float32 frames";
    return false;
  }
  const auto& pixels = std::get<std::shared_ptr<const CpuFrame>>(frame.storage);
  if (!pixels || pixels->width() != frame.width || pixels->height() != frame.height) {
    diagnostic = "VideoFrame metadata does not match its CPU pixel storage";
    return false;
  }
  if (frame.alpha_mode != AlphaMode::Premultiplied) {
    diagnostic = "GPU upload currently requires premultiplied alpha";
    return false;
  }
  if (frame.color.primaries != "bt709" || frame.color.transfer != "linear" ||
      frame.color.matrix != "rgb" || frame.color.range != "full") {
    diagnostic = "GPU upload currently requires linear full-range Rec.709 RGB metadata";
    return false;
  }
  return true;
}

[[nodiscard]] pl_fmt source_format(pl_gpu gpu) {
  const auto required = static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
  return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, required);
}

[[nodiscard]] pl_fmt target_format(pl_gpu gpu) {
  const auto required = static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR |
                                                 PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_BLENDABLE |
                                                 PL_FMT_CAP_HOST_READABLE | PL_FMT_CAP_BLITTABLE);
  return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, required);
}

[[nodiscard]] pl_tex create_render_target(pl_gpu gpu, const int width, const int height,
                                          const bool host_readable) {
  pl_tex_params params{};
  params.w = width;
  params.h = height;
  params.format = target_format(gpu);
  params.sampleable = true;
  params.renderable = true;
  params.blit_dst = true;
  params.host_readable = host_readable;
  params.host_writable = host_readable;
  return pl_tex_create(gpu, &params);
}

struct OpacityHookState final {
  std::string shader_body;
};

[[nodiscard]] pl_hook_res opacity_hook(void* private_data, const pl_hook_params* parameters) {
  const auto* state = static_cast<const OpacityHookState*>(private_data);
  pl_custom_shader shader{};
  shader.description = "Video Editor clip opacity";
  shader.body = state->shader_body.c_str();
  shader.input = PL_SHADER_SIG_COLOR;
  shader.output = PL_SHADER_SIG_COLOR;
  if (!pl_shader_custom(parameters->sh, &shader)) {
    pl_hook_res failed{};
    failed.failed = true;
    return failed;
  }
  pl_hook_res result{};
  result.output = PL_HOOK_SIG_COLOR;
  result.sh = parameters->sh;
  result.repr = parameters->repr;
  result.color = parameters->color;
  result.components = parameters->components;
  result.rect = parameters->rect;
  return result;
}

[[nodiscard]] pl_fmt lut3d_format(pl_gpu gpu) {
  const auto required = static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
  return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, required);
}

[[nodiscard]] pl_fmt curves_format(pl_gpu gpu) {
  const auto required = static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
  return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, required);
}

[[nodiscard]] pl_tex get_or_create_lut_texture(LibplaceboState& state, const Lut3D& lut,
                                               const std::string& cache_key) {
  auto& entry = state.lut_texture_cache[cache_key];
  if (entry.texture != nullptr && entry.lattice_size == lut.size) {
    return entry.texture;
  }
  if (entry.texture != nullptr) {
    pl_tex_destroy(state.gpu, &entry.texture);
    entry.texture = nullptr;
  }
  const pl_fmt format = lut3d_format(state.gpu);
  if (format == nullptr || lut.size < 2 || lut.lattice.empty()) {
    return nullptr;
  }
  const int width = lut.size * lut.size;
  const int height = lut.size;
  std::vector<float> packed(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int blue = 0; blue < lut.size; ++blue) {
    for (int green = 0; green < lut.size; ++green) {
      for (int red = 0; red < lut.size; ++red) {
        const std::size_t lattice_index =
            static_cast<std::size_t>(red) +
            (static_cast<std::size_t>(green) * static_cast<std::size_t>(lut.size)) +
            (static_cast<std::size_t>(blue) * static_cast<std::size_t>(lut.size) *
             static_cast<std::size_t>(lut.size));
        const std::size_t atlas_x = static_cast<std::size_t>(red) +
                                    (static_cast<std::size_t>(green) *
                                     static_cast<std::size_t>(lut.size));
        const std::size_t pixel_index =
            (static_cast<std::size_t>(blue) * static_cast<std::size_t>(width) + atlas_x) * 4U;
        packed[pixel_index + 0U] = lut.lattice[lattice_index][0];
        packed[pixel_index + 1U] = lut.lattice[lattice_index][1];
        packed[pixel_index + 2U] = lut.lattice[lattice_index][2];
        packed[pixel_index + 3U] = 1.0F;
      }
    }
  }
  pl_tex_params params{};
  params.w = width;
  params.h = height;
  params.format = format;
  params.sampleable = true;
  params.host_writable = true;
  params.initial_data = packed.data();
  entry.texture = pl_tex_create(state.gpu, &params);
  entry.lattice_size = lut.size;
  return entry.texture;
}

[[nodiscard]] pl_tex get_or_create_curves_texture(LibplaceboState& state, const ColorCurves& curves,
                                                  const std::string& cache_key) {
  auto& entry = state.curves_texture_cache[cache_key];
  if (entry.texture != nullptr) {
    return entry.texture;
  }
  const pl_fmt format = curves_format(state.gpu);
  if (format == nullptr) {
    return nullptr;
  }
  std::vector<float> packed(static_cast<std::size_t>(kCurveLutSamples) * 4U * 4U, 0.0F);
  const auto bake_channel = [&](const std::vector<CurvePoint>& points, const int row) {
    for (int sample = 0; sample < kCurveLutSamples; ++sample) {
      const float input =
          kCurveLutSamples > 1
              ? static_cast<float>(sample) / static_cast<float>(kCurveLutSamples - 1)
              : 0.0F;
      const float mapped =
          static_cast<float>(evaluate_curve(points, static_cast<double>(input)));
      packed[(static_cast<std::size_t>(row) * static_cast<std::size_t>(kCurveLutSamples) * 4U) +
             (static_cast<std::size_t>(sample) * 4U)] = mapped;
    }
  };
  bake_channel(curves.red, 0);
  bake_channel(curves.green, 1);
  bake_channel(curves.blue, 2);
  bake_channel(curves.luma, 3);
  pl_tex_params params{};
  params.w = kCurveLutSamples;
  params.h = 4;
  params.format = format;
  params.sampleable = true;
  params.host_writable = true;
  params.initial_data = packed.data();
  entry.texture = pl_tex_create(state.gpu, &params);
  return entry.texture;
}

struct ColorGradePassState final {
  std::string header;
  std::string body;
  std::vector<std::string> descriptor_names;
  std::vector<pl_shader_desc> descriptors;
};

[[nodiscard]] pl_hook_res color_grade_hook(void* private_data, const pl_hook_params* parameters) {
  const auto* pass_state = static_cast<const ColorGradePassState*>(private_data);
  pl_custom_shader shader{};
  shader.description = "Video Editor color grade";
  shader.header = pass_state->header.c_str();
  shader.body = pass_state->body.c_str();
  shader.input = PL_SHADER_SIG_COLOR;
  shader.output = PL_SHADER_SIG_COLOR;
  shader.descriptors = pass_state->descriptors.data();
  shader.num_descriptors = static_cast<int>(pass_state->descriptors.size());
  if (!pl_shader_custom(parameters->sh, &shader)) {
    pl_hook_res failed{};
    failed.failed = true;
    return failed;
  }
  pl_hook_res result{};
  result.output = PL_HOOK_SIG_COLOR;
  result.sh = parameters->sh;
  result.repr = parameters->repr;
  result.color = parameters->color;
  result.components = parameters->components;
  result.rect = parameters->rect;
  return result;
}

[[nodiscard]] bool build_color_grade_pass(LibplaceboState& state, const GpuColorGrade& grade,
                                          ColorGradePassState& pass_state) {
  pass_state.header =
      "vec3 ve_unpremultiply(vec4 c) {\n"
      "  float a = max(c.a, 1.0e-6);\n"
      "  return clamp(c.rgb / a, 0.0, 1.0);\n"
      "}\n"
      "float ve_curve_sample(sampler2D curves, int row, float x) {\n"
      "  const float n = 256.0;\n"
      "  float u = (x * (n - 1.0) + 0.5) / n;\n"
      "  float v = (float(row) + 0.5) / 4.0;\n"
      "  return texture(curves, vec2(u, v)).r;\n"
      "}\n"
      "vec3 ve_map_domain(vec3 c, vec3 dmin, vec3 dmax) {\n"
      "  return clamp((c - dmin) / (dmax - dmin), 0.0, 1.0);\n"
      "}\n"
      "vec3 ve_lut_fetch(sampler2D atlas, float lut_size, vec3 index) {\n"
      "  vec2 atlas_size = vec2(lut_size * lut_size, lut_size);\n"
      "  float x = index.r + index.g * lut_size;\n"
      "  float y = index.b;\n"
      "  vec2 uv = (vec2(x, y) + 0.5) / atlas_size;\n"
      "  return texture(atlas, uv).rgb;\n"
      "}\n"
      "vec3 ve_sample_lut3d(sampler2D atlas, float lut_size, vec3 mapped) {\n"
      "  float scale = lut_size - 1.0;\n"
      "  vec3 scaled = mapped * scale;\n"
      "  vec3 base = floor(scaled);\n"
      "  vec3 frac = scaled - base;\n"
      "  base = clamp(base, vec3(0.0), vec3(lut_size - 2.0));\n"
      "  vec3 c000 = ve_lut_fetch(atlas, lut_size, base + vec3(0.0, 0.0, 0.0));\n"
      "  vec3 c100 = ve_lut_fetch(atlas, lut_size, base + vec3(1.0, 0.0, 0.0));\n"
      "  vec3 c010 = ve_lut_fetch(atlas, lut_size, base + vec3(0.0, 1.0, 0.0));\n"
      "  vec3 c110 = ve_lut_fetch(atlas, lut_size, base + vec3(1.0, 1.0, 0.0));\n"
      "  vec3 c001 = ve_lut_fetch(atlas, lut_size, base + vec3(0.0, 0.0, 1.0));\n"
      "  vec3 c101 = ve_lut_fetch(atlas, lut_size, base + vec3(1.0, 0.0, 1.0));\n"
      "  vec3 c011 = ve_lut_fetch(atlas, lut_size, base + vec3(0.0, 1.0, 1.0));\n"
      "  vec3 c111 = ve_lut_fetch(atlas, lut_size, base + vec3(1.0, 1.0, 1.0));\n"
      "  if (frac.r >= frac.g) {\n"
      "    if (frac.g >= frac.b) {\n"
      "      return mix(mix(mix(c000, c100, frac.r), c110, frac.g), c111, frac.b);\n"
      "    } else if (frac.r >= frac.b) {\n"
      "      return mix(mix(mix(c000, c100, frac.r), c101, frac.b), c111, frac.g);\n"
      "    }\n"
      "    return mix(mix(mix(c000, c001, frac.b), c101, frac.r), c111, frac.g);\n"
      "  }\n"
      "  if (frac.g >= frac.b) {\n"
      "    return mix(mix(mix(c000, c010, frac.g), c110, frac.r), c111, frac.b);\n"
      "  } else if (frac.r >= frac.b) {\n"
      "    return mix(mix(mix(c000, c010, frac.g), c011, frac.b), c111, frac.r);\n"
      "  }\n"
      "  return mix(mix(mix(c000, c001, frac.b), c011, frac.g), c111, frac.r);\n"
      "}\n"
      "vec3 ve_apply_curves(sampler2D curves, vec3 rgb) {\n"
      "  vec3 curved = vec3(\n"
      "    ve_curve_sample(curves, 0, rgb.r),\n"
      "    ve_curve_sample(curves, 1, rgb.g),\n"
      "    ve_curve_sample(curves, 2, rgb.b));\n"
      "  float luma = dot(curved, vec3(0.2126, 0.7152, 0.0722));\n"
      "  float adjusted = ve_curve_sample(curves, 3, luma);\n"
      "  if (luma > 0.0) {\n"
      "    curved *= (adjusted / luma);\n"
      "  } else {\n"
      "    curved = vec3(adjusted);\n"
      "  }\n"
      "  return clamp(curved, 0.0, 1.0);\n"
      "}\n";

  std::ostringstream body;
  body.imbue(std::locale::classic());
  body.precision(17);
  body << "vec3 rgb = ve_unpremultiply(color);\n";

  int lut_index = 0;
  int curves_index = 0;
  for (const GpuColorGradeOp& op : grade.ops) {
    if (op.kind == GpuColorGradeOp::Kind::Lut) {
      if (op.lut == nullptr) {
        return false;
      }
      const pl_tex texture = get_or_create_lut_texture(state, *op.lut, op.lut_cache_key);
      if (texture == nullptr) {
        return false;
      }
      const std::string name = "lut" + std::to_string(lut_index);
      pass_state.descriptor_names.push_back(name);
      pl_shader_desc descriptor{};
      descriptor.desc.name = pass_state.descriptor_names.back().c_str();
      descriptor.desc.type = PL_DESC_SAMPLED_TEX;
      descriptor.binding.object = texture;
      descriptor.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
      descriptor.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
      pass_state.descriptors.push_back(descriptor);

      body << "{\n"
           << "  vec3 mapped = ve_map_domain(rgb, vec3(" << op.lut->domain_min[0] << ", "
           << op.lut->domain_min[1] << ", " << op.lut->domain_min[2] << "), vec3("
           << op.lut->domain_max[0] << ", " << op.lut->domain_max[1] << ", "
           << op.lut->domain_max[2] << "));\n"
           << "  rgb = ve_sample_lut3d(" << name << ", float(" << op.lut->size << "), mapped);\n"
           << "}\n";
      ++lut_index;
    } else {
      const pl_tex texture = get_or_create_curves_texture(state, op.curves, op.curves_cache_key);
      if (texture == nullptr) {
        return false;
      }
      const std::string name = "curves" + std::to_string(curves_index);
      pass_state.descriptor_names.push_back(name);
      pl_shader_desc descriptor{};
      descriptor.desc.name = pass_state.descriptor_names.back().c_str();
      descriptor.desc.type = PL_DESC_SAMPLED_TEX;
      descriptor.binding.object = texture;
      descriptor.binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
      descriptor.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
      pass_state.descriptors.push_back(descriptor);
      body << "rgb = ve_apply_curves(" << name << ", rgb);\n";
      ++curves_index;
    }
  }
  body << "color.rgb = rgb * color.a;\n";
  pass_state.body = body.str();
  return true;
}

[[nodiscard]] pl_rect2df mapped_crop(const GpuLayer& layer, const int output_width,
                                     const int output_height, const std::uint32_t sequence_width,
                                     const std::uint32_t sequence_height,
                                     const double rotation_radians) {
  const double preview_x = static_cast<double>(output_width) / static_cast<double>(sequence_width);
  const double preview_y =
      static_cast<double>(output_height) / static_cast<double>(sequence_height);
  const bool has_rotation = std::abs(rotation_radians) > 1.0e-9;
  const double canvas_x = has_rotation ? (static_cast<double>(output_width) - 1.0) * 0.5
                                       : static_cast<double>(output_width) * 0.5;
  const double canvas_y = has_rotation ? (static_cast<double>(output_height) - 1.0) * 0.5
                                       : static_cast<double>(output_height) * 0.5;
  const double pivot_x = canvas_x + (layer.transform.position.x * preview_x);
  const double pivot_y = canvas_y + (layer.transform.position.y * preview_y);
  const double cosine = std::cos(rotation_radians);
  const double sine = std::sin(rotation_radians);
  const double pivot_delta_x = pivot_x - canvas_x;
  const double pivot_delta_y = pivot_y - canvas_y;
  // libplacebo's distortion rotates around the output canvas center. Move the
  // unrotated pivot by the inverse rotation so that the final pivot lands at
  // the edit model's (sequence center + clip position) point.
  const double unrotated_pivot_x = canvas_x + (cosine * pivot_delta_x) + (sine * pivot_delta_y);
  const double unrotated_pivot_y = canvas_y - (sine * pivot_delta_x) + (cosine * pivot_delta_y);

  const double source_width = static_cast<double>(layer.image.width());
  const double source_height = static_cast<double>(layer.image.height());
  const double source_anchor_x = has_rotation
                                     ? layer.transform.anchor_x * (source_width - 1.0)
                                     : (layer.transform.anchor_x * (source_width - 1.0)) + 0.5;
  const double source_anchor_y = has_rotation
                                     ? layer.transform.anchor_y * (source_height - 1.0)
                                     : (layer.transform.anchor_y * (source_height - 1.0)) + 0.5;
  const double source_x0 = layer.transform.crop_left * source_width;
  const double source_y0 = layer.transform.crop_top * source_height;
  const double source_x1 = (1.0 - layer.transform.crop_right) * source_width;
  const double source_y1 = (1.0 - layer.transform.crop_bottom) * source_height;
  return {
      .x0 = static_cast<float>(unrotated_pivot_x +
                               (layer.transform.scale.x * (source_x0 - source_anchor_x))),
      .y0 = static_cast<float>(unrotated_pivot_y +
                               (layer.transform.scale.y * (source_y0 - source_anchor_y))),
      .x1 = static_cast<float>(unrotated_pivot_x +
                               (layer.transform.scale.x * (source_x1 - source_anchor_x))),
      .y1 = static_cast<float>(unrotated_pivot_y +
                               (layer.transform.scale.y * (source_y1 - source_anchor_y))),
  };
}

[[nodiscard]] bool valid_timeline_transform(const GpuLayer& layer, std::string& diagnostic) {
  const auto& transform = layer.transform;
  const std::array<double, 12> values{
      transform.position.x,       transform.position.y, transform.scale.x,     transform.scale.y,
      transform.rotation_degrees, transform.anchor_x,   transform.anchor_y,    transform.crop_left,
      transform.crop_top,         transform.crop_right, transform.crop_bottom, transform.opacity,
  };
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      transform.scale.x == 0.0 || transform.scale.y == 0.0) {
    diagnostic = "GPU timeline transforms must be finite and use non-zero scale";
    return false;
  }
  if (transform.crop_left < 0.0 || transform.crop_top < 0.0 || transform.crop_right < 0.0 ||
      transform.crop_bottom < 0.0 || transform.crop_left + transform.crop_right > 1.0 ||
      transform.crop_top + transform.crop_bottom > 1.0 || transform.opacity < 0.0 ||
      transform.opacity > 1.0) {
    diagnostic = "GPU timeline crop and opacity values are outside their normalized ranges";
    return false;
  }
  return true;
}

[[nodiscard]] bool download_texture(pl_gpu gpu, pl_tex texture, const int width, const int height,
                                    CpuFrame& frame) {
  (void)height;
  pl_tex_transfer_params transfer{};
  transfer.tex = texture;
  transfer.row_pitch = static_cast<std::size_t>(width) * 4U * sizeof(float);
  transfer.ptr = frame.pixels().data();
  return pl_tex_download(gpu, &transfer);
}

[[nodiscard]] bool upload_texture(pl_gpu gpu, pl_tex texture, const CpuFrame& frame) {
  pl_tex_transfer_params transfer{};
  transfer.tex = texture;
  transfer.row_pitch = static_cast<std::size_t>(frame.width()) * 4U * sizeof(float);
  transfer.ptr = const_cast<float*>(frame.pixels().data());
  return pl_tex_upload(gpu, &transfer);
}

[[nodiscard]] bool copy_texture(pl_renderer renderer, pl_gpu gpu, pl_tex destination,
                                pl_tex source, const int width, const int height) {
  (void)gpu;
  pl_frame copy_source = make_frame(source, width, height);
  pl_frame copy_destination = make_frame(destination, width, height);
  pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_bilinear;
  params.downscaler = &pl_filter_bilinear;
  params.background = PL_CLEAR_SKIP;
  params.border = PL_CLEAR_SKIP;
  return pl_render_image(renderer, &copy_source, &copy_destination, &params);
}

} // namespace

struct GpuImage::Storage final {
  std::shared_ptr<LibplaceboState> device;
  pl_tex texture{nullptr};
  int width{0};
  int height{0};
  edit::Time timestamp{};
  edit::Time duration{};
  FrameColor color{};
  AlphaMode alpha_mode{AlphaMode::Premultiplied};

  ~Storage() {
    if (!device || texture == nullptr) {
      return;
    }
    std::scoped_lock lock(device->mutex);
    pl_tex_destroy(device->gpu, &texture);
  }
};

struct GpuRenderer::Impl final {
  std::shared_ptr<LibplaceboState> state;
};

GpuImage::GpuImage(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}

bool GpuImage::valid() const noexcept {
  return storage_ && storage_->texture != nullptr;
}
int GpuImage::width() const noexcept {
  return storage_ ? storage_->width : 0;
}
int GpuImage::height() const noexcept {
  return storage_ ? storage_->height : 0;
}
edit::Time GpuImage::timestamp() const noexcept {
  return storage_ ? storage_->timestamp : edit::Time{};
}
edit::Time GpuImage::duration() const noexcept {
  return storage_ ? storage_->duration : edit::Time{};
}
const FrameColor& GpuImage::color() const noexcept {
  static const FrameColor empty{};
  return storage_ ? storage_->color : empty;
}
AlphaMode GpuImage::alpha_mode() const noexcept {
  return storage_ ? storage_->alpha_mode : AlphaMode::Premultiplied;
}

GpuRenderer::GpuRenderer(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
GpuRenderer::~GpuRenderer() = default;
GpuRenderer::GpuRenderer(GpuRenderer&&) noexcept = default;
GpuRenderer& GpuRenderer::operator=(GpuRenderer&&) noexcept = default;

std::unique_ptr<GpuRenderer> GpuRenderer::create(const GpuOptions& options) {
  auto state = std::make_shared<LibplaceboState>();
  const GpuBuildCapabilities build = gpu_build_capabilities();
  const GpuBackendSelection selection = select_gpu_backend(build, options.preferred_backend);
  state->capabilities.backend = selection.backend;
  state->capabilities.api_version = build.libplacebo_version;
  if (!selection.supported) {
    state->capabilities.diagnostic = selection.diagnostic;
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }
  if (options.presentation.surface != 0 && options.presentation.backend != GpuBackendKind::Auto &&
      options.presentation.backend != selection.backend) {
    state->capabilities.diagnostic = "presentation surface backend does not match selected GPU";
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }

  pl_log_params log_params{};
  log_params.log_level = options.enable_debug_layers ? PL_LOG_DEBUG : PL_LOG_WARN;
  state->log = pl_log_create(PL_API_VER, &log_params);
  if (state->log == nullptr) {
    state->capabilities.diagnostic = "libplacebo log context creation failed";
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }

#if defined(_WIN32) && defined(PL_HAVE_D3D11)
  pl_d3d11_params device_params{};
  device_params.allow_software = options.allow_software;
  device_params.debug = options.enable_debug_layers;
  device_params.min_feature_level = D3D_FEATURE_LEVEL_11_0;
  state->d3d11 = pl_d3d11_create(state->log, &device_params);
  if (state->d3d11 != nullptr) {
    state->gpu = state->d3d11->gpu;
    state->capabilities.software_device = state->d3d11->software;
  }
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
  pl_vulkan_params device_params{};
  device_params.allow_software = options.allow_software;
  device_params.async_transfer = true;
  device_params.async_compute = true;
  device_params.queue_count = 1;
  // Do not import a Qt VkInstance/VkSurfaceKHR. libplacebo then calls
  // vkGetPhysicalDeviceSurfaceSupportKHR during device selection, which
  // SIGSEGVs in NVIDIA's driver and can fatal the Wayland connection
  // (zwp_linux_dmabuf_v1.get_surface_feedback). Offscreen GPU still works
  // with a libplacebo-owned instance.
  state->vulkan = pl_vulkan_create(state->log, &device_params);
  if (state->vulkan != nullptr) {
    state->gpu = state->vulkan->gpu;
    state->capabilities.software_device =
        options.allow_software ? std::nullopt : std::optional<bool>(false);
  }
#endif

  if (state->gpu == nullptr) {
    state->capabilities.diagnostic =
        options.allow_software ? "libplacebo could not create a GPU device"
                               : "libplacebo could not create a supported hardware GPU device";
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }
  if (source_format(state->gpu) == nullptr || target_format(state->gpu) == nullptr) {
    state->capabilities.diagnostic =
        "GPU lacks a renderable/readable linear RGBA float32 format required by the foundation";
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }
  state->renderer = pl_renderer_create(state->log, state->gpu);
  if (state->renderer == nullptr) {
    state->capabilities.diagnostic = "libplacebo renderer creation failed";
    return std::unique_ptr<GpuRenderer>(
        new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
  }

  if (options.presentation.surface != 0) {
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
    pl_d3d11_swapchain_params swapchain_params{};
    swapchain_params.window = reinterpret_cast<HWND>(options.presentation.surface);
    swapchain_params.width = std::max(1, options.presentation.width);
    swapchain_params.height = std::max(1, options.presentation.height);
    state->swapchain = pl_d3d11_create_swapchain(state->d3d11, &swapchain_params);
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
    // See device creation: Qt Vulkan surfaces cannot be imported safely.
    // #region agent log
    {
      std::ofstream out("/home/sambhav/Projects/VideoEditor/.cursor/debug-4d158f.log",
                        std::ios::app);
      if (out) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        out << "{\"sessionId\":\"4d158f\",\"runId\":\"pre-fix\",\"hypothesisId\":\"B\","
               "\"location\":\"gpu_backend.cpp:create\",\"message\":\"skipped Qt Vulkan surface "
               "import\",\"data\":{\"surface\":"
            << (options.presentation.surface != 0) << "},\"timestamp\":" << ms << "}\n";
      }
    }
    // #endregion
#endif
  }

  state->capabilities.state = GpuRuntimeState::Ready;
  state->capabilities.offscreen_rendering = true;
  state->capabilities.presentation = state->swapchain != nullptr;
  state->capabilities.diagnostic =
      options.presentation.surface != 0 && state->swapchain == nullptr
#if defined(__linux__)
          ? "offscreen GPU rendering is ready, but native presentation was skipped because "
            "the window Vulkan surface cannot be imported safely"
#else
          ? "offscreen GPU rendering is ready, but presentation swapchain creation failed"
#endif
          : "GPU rendering is ready";
  return std::unique_ptr<GpuRenderer>(
      new GpuRenderer(std::make_unique<Impl>(Impl{.state = std::move(state)})));
}

GpuCapabilities GpuRenderer::capabilities() const {
  if (!implementation_ || !implementation_->state) {
    GpuCapabilities missing;
    missing.diagnostic = "GPU renderer has no implementation";
    return missing;
  }
  std::scoped_lock lock(implementation_->state->mutex);
  return implementation_->state->capabilities;
}

RenderResult<GpuImage> GpuRenderer::upload(const VideoFrame& frame) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
  }
  std::string diagnostic;
  if (!valid_cpu_frame(frame, diagnostic)) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame, .message = std::move(diagnostic)});
  }

  const auto& source = std::get<std::shared_ptr<const CpuFrame>>(frame.storage);
  pl_tex_params params{};
  params.w = frame.width;
  params.h = frame.height;
  params.format = source_format(state->gpu);
  params.sampleable = true;
  params.host_writable = true;
  params.host_readable = true;
  params.initial_data = source->pixels().data();
  pl_tex texture = pl_tex_create(state->gpu, &params);
  if (texture == nullptr) {
    if (detect_device_loss(*state, "uploading a frame")) {
      return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuUploadFailed,
         .message = "libplacebo could not allocate or upload the RGBA frame"});
  }

  auto storage = std::make_shared<GpuImage::Storage>();
  storage->device = state;
  storage->texture = texture;
  storage->width = frame.width;
  storage->height = frame.height;
  storage->timestamp = frame.timestamp;
  storage->duration = frame.duration;
  storage->color = frame.color;
  storage->alpha_mode = frame.alpha_mode;
  return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
}

RenderResult<GpuImage> GpuRenderer::apply_color_grade(const GpuImage& source,
                                                      const GpuColorGrade& grade) {
  if (grade.empty()) {
    return RenderResult<GpuImage>::success(source);
  }
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  if (!source.storage_ || source.storage_->texture == nullptr) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU color grade requires a valid source image"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
  }
  if (source.storage_->device.get() != state.get()) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU color grade source must be owned by this renderer device"});
  }

  ColorGradePassState pass_state;
  if (!build_color_grade_pass(*state, grade, pass_state)) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuRenderFailed,
         .message = "GPU could not prepare LUT/curves textures for color grading"});
  }

  pl_tex output_texture =
      create_render_target(state->gpu, source.width(), source.height(), true);
  if (output_texture == nullptr) {
    if (detect_device_loss(*state, "allocating a color grade target")) {
      return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuRenderFailed,
         .message = "GPU could not allocate a color grade target"});
  }

  pl_frame source_frame =
      make_frame(source.storage_->texture, source.width(), source.height());
  pl_frame target_frame = make_frame(output_texture, source.width(), source.height());

  pl_hook hook{};
  hook.stages = PL_HOOK_PRE_OUTPUT;
  hook.input = PL_HOOK_SIG_COLOR;
  hook.priv = &pass_state;
  hook.hook = &color_grade_hook;
  hook.signature = 0x5645434f4c4f5244ULL;
  const pl_hook* hooks[]{&hook};

  pl_render_params params = pl_render_default_params;
  params.upscaler = &pl_filter_bilinear;
  params.downscaler = &pl_filter_bilinear;
  params.background = PL_CLEAR_SKIP;
  params.border = PL_CLEAR_SKIP;
  params.hooks = hooks;
  params.num_hooks = 1;

  if (!pl_render_image(state->renderer, &source_frame, &target_frame, &params)) {
    pl_tex_destroy(state->gpu, &output_texture);
    if (detect_device_loss(*state, "applying GPU LUT/curves color grade")) {
      return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuRenderFailed,
         .message = "libplacebo failed to apply GPU LUT/curves color grade"});
  }

  auto storage = std::make_shared<GpuImage::Storage>();
  storage->device = state;
  storage->texture = output_texture;
  storage->width = source.width();
  storage->height = source.height();
  storage->timestamp = source.timestamp();
  storage->duration = source.duration();
  storage->color = source.color();
  storage->alpha_mode = source.alpha_mode();
  return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
}

RenderResult<GpuImage> GpuRenderer::composite(const std::span<const GpuImage> layers,
                                              const int width, const int height,
                                              const edit::Time timestamp,
                                              const edit::Time duration) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
  }
  if (layers.empty() || width <= 0 || height <= 0 || duration.isNegative()) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message =
             "GPU composite requires layers, positive dimensions, and non-negative duration"});
  }
  for (const GpuImage& layer : layers) {
    if (!layer.storage_ || layer.storage_->texture == nullptr ||
        layer.storage_->device.get() != state.get()) {
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuInvalidFrame,
           .message = "GPU composite layers must be valid and owned by this renderer device"});
    }
  }

  pl_tex_params target_params{};
  target_params.w = width;
  target_params.h = height;
  target_params.format = target_format(state->gpu);
  target_params.sampleable = true;
  target_params.renderable = true;
  target_params.host_readable = true;
  pl_tex target_texture = pl_tex_create(state->gpu, &target_params);
  if (target_texture == nullptr) {
    if (detect_device_loss(*state, "allocating a composite target")) {
      return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuRenderFailed,
         .message = "GPU could not allocate a renderable RGBA float32 target"});
  }

  const pl_frame target = make_frame(target_texture, width, height);
  constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
  pl_frame_clear_rgba(state->gpu, &target, transparent.data());
  const pl_blend_params source_over{
      .src_rgb = PL_BLEND_ONE,
      .dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA,
      .src_alpha = PL_BLEND_ONE,
      .dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA,
  };
  pl_render_params render_params = pl_render_default_params;
  render_params.upscaler = &pl_filter_bilinear;
  render_params.downscaler = &pl_filter_bilinear;
  render_params.blend_params = &source_over;
  render_params.background = PL_CLEAR_SKIP;
  render_params.border = PL_CLEAR_SKIP;

  for (const GpuImage& layer : layers) {
    const pl_frame source =
        make_frame(layer.storage_->texture, layer.storage_->width, layer.storage_->height);
    if (!pl_render_image(state->renderer, &source, &target, &render_params)) {
      pl_tex_destroy(state->gpu, &target_texture);
      if (detect_device_loss(*state, "compositing a frame")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "libplacebo failed to composite the frame"});
    }
  }

  auto storage = std::make_shared<GpuImage::Storage>();
  storage->device = state;
  storage->texture = target_texture;
  storage->width = width;
  storage->height = height;
  storage->timestamp = timestamp;
  storage->duration = duration;
  storage->color = {};
  storage->alpha_mode = AlphaMode::Premultiplied;
  return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
}

RenderResult<GpuImage> GpuRenderer::composite_timeline(const std::span<const GpuLayer> layers,
                                                       const int width, const int height,
                                                       const std::uint32_t sequence_width,
                                                       const std::uint32_t sequence_height,
                                                       const edit::Time timestamp,
                                                       const edit::Time duration,
                                                       const GpuImage* background) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
  }
  if (width <= 0 || height <= 0 || sequence_width == 0 || sequence_height == 0 ||
      duration.isNegative()) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU timeline composite requires positive dimensions and duration"});
  }
  if (background != nullptr &&
      (!background->storage_ || background->storage_->texture == nullptr ||
       background->storage_->device.get() != state.get() || background->width() != width ||
       background->height() != height)) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU timeline background must be a same-size image owned by this renderer"});
  }
  for (const GpuLayer& layer : layers) {
    std::string diagnostic;
    if (!layer.image.storage_ || layer.image.storage_->texture == nullptr ||
        layer.image.storage_->device.get() != state.get()) {
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuInvalidFrame,
           .message = "GPU timeline layers must be valid and owned by this renderer device"});
    }
    if (!valid_timeline_transform(layer, diagnostic)) {
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuInvalidFrame, .message = std::move(diagnostic)});
    }
  }

  if (layers.empty()) {
    if (background == nullptr) {
      pl_tex output_texture = create_render_target(state->gpu, width, height, true);
      if (output_texture == nullptr) {
        if (detect_device_loss(*state, "allocating an empty timeline target")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuRenderFailed,
             .message = "GPU could not allocate the timeline target"});
      }
      constexpr std::array<float, 4> opaque_black{0.0F, 0.0F, 0.0F, 1.0F};
      pl_tex_clear(state->gpu, output_texture, opaque_black.data());
      auto storage = std::make_shared<GpuImage::Storage>();
      storage->device = state;
      storage->texture = output_texture;
      storage->width = width;
      storage->height = height;
      storage->timestamp = timestamp;
      storage->duration = duration;
      storage->color = {};
      storage->alpha_mode = AlphaMode::Premultiplied;
      return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
    }
    pl_tex output_texture = create_render_target(state->gpu, width, height, true);
    if (output_texture == nullptr) {
      if (detect_device_loss(*state, "allocating a background timeline target")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "GPU could not allocate the timeline target"});
    }
    if (!copy_texture(state->renderer, state->gpu, output_texture, background->storage_->texture,
                      width, height)) {
      pl_tex_destroy(state->gpu, &output_texture);
      if (detect_device_loss(*state, "copying a timeline background")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "GPU could not copy the timeline background"});
    }
    auto storage = std::make_shared<GpuImage::Storage>();
    storage->device = state;
    storage->texture = output_texture;
    storage->width = width;
    storage->height = height;
    storage->timestamp = timestamp;
    storage->duration = duration;
    storage->color = {};
    storage->alpha_mode = AlphaMode::Premultiplied;
    return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
  }

  pl_tex output_texture = create_render_target(state->gpu, width, height, true);
  if (output_texture == nullptr) {
    if (detect_device_loss(*state, "allocating a timeline target")) {
      return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::GpuRenderFailed,
         .message = "GPU could not allocate the timeline target"});
  }
  const pl_frame output = make_frame(output_texture, width, height);
  constexpr std::array<float, 4> opaque_black{0.0F, 0.0F, 0.0F, 1.0F};
  if (background != nullptr) {
    if (!copy_texture(state->renderer, state->gpu, output_texture, background->storage_->texture,
                      width, height)) {
      pl_tex_destroy(state->gpu, &output_texture);
      if (detect_device_loss(*state, "seeding a timeline background")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "GPU could not seed the timeline background"});
    }
  } else {
    pl_tex_clear(state->gpu, output_texture, opaque_black.data());
  }

  const pl_blend_params source_over{
      .src_rgb = PL_BLEND_ONE,
      .dst_rgb = PL_BLEND_ONE_MINUS_SRC_ALPHA,
      .src_alpha = PL_BLEND_ONE,
      .dst_alpha = PL_BLEND_ONE_MINUS_SRC_ALPHA,
  };
  pl_render_params final_params = pl_render_default_params;
  final_params.upscaler = &pl_filter_bilinear;
  final_params.downscaler = &pl_filter_bilinear;
  final_params.blend_params = &source_over;
  final_params.background = PL_CLEAR_SKIP;
  final_params.border = PL_CLEAR_SKIP;

  const auto uses_moved_pivot_rotation = [](const GpuLayer& layer) {
    return std::abs(layer.transform.rotation_degrees) > 1.0e-9 &&
           (std::abs(layer.transform.position.x) > 1.0e-9 ||
            std::abs(layer.transform.position.y) > 1.0e-9 ||
            std::abs(layer.transform.anchor_x - 0.5) > 1.0e-9 ||
            std::abs(layer.transform.anchor_y - 0.5) > 1.0e-9);
  };

  for (const GpuLayer& layer : layers) {
    if (layer.transform.opacity <= 0.0 ||
        layer.transform.crop_left + layer.transform.crop_right >= 1.0 ||
        layer.transform.crop_top + layer.transform.crop_bottom >= 1.0) {
      continue;
    }

    if (uses_moved_pivot_rotation(layer)) {
      auto destination = std::make_shared<CpuFrame>(width, height);
      auto source_pixels = std::make_shared<CpuFrame>(layer.image.width(), layer.image.height());
      if (!download_texture(state->gpu, output_texture, width, height, *destination) ||
          !download_texture(state->gpu, layer.image.storage_->texture, layer.image.width(),
                            layer.image.height(), *source_pixels)) {
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "reading back a moved-pivot rotation layer")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuDownloadFailed,
             .message = "GPU could not read back textures for moved-pivot rotation compositing"});
      }
      edit::Clip clip;
      clip.transform = layer.transform;
      clip.blend_mode = layer.blend_mode;
      composite_clip_onto_frame(*source_pixels, *destination, clip, sequence_width, sequence_height);
      if (!upload_texture(state->gpu, output_texture, *destination)) {
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "uploading a moved-pivot rotation layer")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUploadFailed,
             .message = "GPU could not upload the moved-pivot rotation result"});
      }
      continue;
    }

    pl_tex unrotated_texture = create_render_target(state->gpu, width, height, true);
    if (unrotated_texture == nullptr) {
      pl_tex_destroy(state->gpu, &output_texture);
      if (detect_device_loss(*state, "allocating a transformed clip target")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "GPU could not allocate a transformed clip target"});
    }

    pl_frame source =
        make_frame(layer.image.storage_->texture, layer.image.width(), layer.image.height());
    source.crop = {
        .x0 = static_cast<float>(layer.transform.crop_left * layer.image.width()),
        .y0 = static_cast<float>(layer.transform.crop_top * layer.image.height()),
        .x1 = static_cast<float>((1.0 - layer.transform.crop_right) * layer.image.width()),
        .y1 = static_cast<float>((1.0 - layer.transform.crop_bottom) * layer.image.height()),
    };
    pl_frame unrotated = make_frame(unrotated_texture, width, height);
    const double radians = layer.transform.rotation_degrees * std::numbers::pi / 180.0;
    unrotated.crop = mapped_crop(layer, width, height, sequence_width, sequence_height, radians);
    constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
    pl_tex_clear(state->gpu, unrotated_texture, transparent.data());

    OpacityHookState opacity_state;
    std::ostringstream opacity_body;
    opacity_body.imbue(std::locale::classic());
    opacity_body.precision(17);
    // PRE_OUTPUT uses independent alpha; changing alpha alone lets the
    // renderer premultiply RGB exactly once before framebuffer blending.
    opacity_body << "color.a *= " << layer.transform.opacity << ";";
    opacity_state.shader_body = opacity_body.str();
    pl_hook opacity{};
    opacity.stages = PL_HOOK_PRE_OUTPUT;
    opacity.input = PL_HOOK_SIG_COLOR;
    opacity.priv = &opacity_state;
    opacity.hook = &opacity_hook;
    opacity.signature =
        0x56454f5041434954ULL ^ std::bit_cast<std::uint64_t>(layer.transform.opacity);
    const pl_hook* hooks[]{&opacity};

    pl_render_params layer_params = pl_render_default_params;
    layer_params.upscaler = &pl_filter_bilinear;
    layer_params.downscaler = &pl_filter_bilinear;
    layer_params.background = PL_CLEAR_SKIP;
    layer_params.border = PL_CLEAR_SKIP;
    layer_params.hooks = hooks;
    layer_params.num_hooks = 1;
    if (!pl_render_image(state->renderer, &source, &unrotated, &layer_params)) {
      pl_tex_destroy(state->gpu, &unrotated_texture);
      pl_tex_destroy(state->gpu, &output_texture);
      if (detect_device_loss(*state, "transforming a timeline clip")) {
        return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuRenderFailed,
           .message = "libplacebo failed to transform timeline clip geometry or opacity"});
    }

    pl_tex layer_texture = unrotated_texture;
    if (std::abs(layer.transform.rotation_degrees) > 1.0e-9) {
      pl_tex rotated_texture = create_render_target(state->gpu, width, height, true);
      if (rotated_texture == nullptr) {
        pl_tex_destroy(state->gpu, &unrotated_texture);
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "allocating a rotated clip target")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuRenderFailed,
             .message = "GPU could not allocate a rotated clip target"});
      }
      pl_frame rotation_source = make_frame(unrotated_texture, width, height);
      pl_frame rotation_target = make_frame(rotated_texture, width, height);
      pl_tex_clear(state->gpu, rotated_texture, transparent.data());
      const float cosine = static_cast<float>(std::cos(radians));
      const float sine = static_cast<float>(std::sin(radians));
      pl_distort_params distortion = pl_distort_default_params;
      distortion.transform.mat.m[0][0] = cosine;
      // libplacebo's transform is expressed in sampling space, so use the
      // inverse matrix to produce the edit model's clockwise visual rotation.
      distortion.transform.mat.m[0][1] = sine;
      distortion.transform.mat.m[1][0] = -sine;
      distortion.transform.mat.m[1][1] = cosine;
      distortion.constrain = false;
      distortion.bicubic = false;
      distortion.alpha_mode = PL_ALPHA_PREMULTIPLIED;
      pl_render_params rotation_params = pl_render_default_params;
      rotation_params.upscaler = &pl_filter_bilinear;
      rotation_params.downscaler = &pl_filter_bilinear;
      rotation_params.background = PL_CLEAR_SKIP;
      rotation_params.border = PL_CLEAR_SKIP;
      rotation_params.distort_params = &distortion;
      if (!pl_render_image(state->renderer, &rotation_source, &rotation_target, &rotation_params)) {
        pl_tex_destroy(state->gpu, &rotated_texture);
        pl_tex_destroy(state->gpu, &unrotated_texture);
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "rotating a timeline clip")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuRenderFailed,
             .message = "libplacebo failed to rotate a timeline clip"});
      }
      pl_tex_destroy(state->gpu, &unrotated_texture);
      layer_texture = rotated_texture;
    }

    const pl_frame final_source = make_frame(layer_texture, width, height);
    if (layer.blend_mode == edit::BlendMode::Normal) {
      if (!pl_render_image(state->renderer, &final_source, &output, &final_params)) {
        pl_tex_destroy(state->gpu, &layer_texture);
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "compositing a transformed timeline clip")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuRenderFailed,
             .message = "libplacebo failed to composite a transformed timeline clip"});
      }
    } else {
      auto destination = std::make_shared<CpuFrame>(width, height);
      auto source_pixels = std::make_shared<CpuFrame>(width, height);
      if (!download_texture(state->gpu, output_texture, width, height, *destination) ||
          !download_texture(state->gpu, layer_texture, width, height, *source_pixels)) {
        pl_tex_destroy(state->gpu, &layer_texture);
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "reading back a custom blend")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuDownloadFailed,
             .message = "GPU could not read back textures for custom blend compositing"});
      }
      composite_blend_frame(*destination, *source_pixels, layer.blend_mode);
      if (!upload_texture(state->gpu, output_texture, *destination)) {
        pl_tex_destroy(state->gpu, &layer_texture);
        pl_tex_destroy(state->gpu, &output_texture);
        if (detect_device_loss(*state, "uploading a custom blend")) {
          return RenderResult<GpuImage>::failure(unavailable_error(state->capabilities));
        }
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUploadFailed,
             .message = "GPU could not upload the custom blend result"});
      }
    }
    pl_tex_destroy(state->gpu, &layer_texture);
  }

  auto storage = std::make_shared<GpuImage::Storage>();
  storage->device = state;
  storage->texture = output_texture;
  storage->width = width;
  storage->height = height;
  storage->timestamp = timestamp;
  storage->duration = duration;
  storage->color = {};
  storage->alpha_mode = AlphaMode::Premultiplied;
  return RenderResult<GpuImage>::success(GpuImage(std::move(storage)));
}

RenderResult<VideoFrame> GpuRenderer::download(const GpuImage& image) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<VideoFrame>::failure(unavailable_error(state->capabilities));
  }
  if (!image.storage_ || image.storage_->texture == nullptr ||
      image.storage_->device.get() != state.get()) {
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU readback requires an image owned by this renderer device"});
  }

  auto cpu = std::make_shared<CpuFrame>(image.storage_->width, image.storage_->height);
  pl_tex_transfer_params transfer{};
  transfer.tex = image.storage_->texture;
  transfer.row_pitch = static_cast<std::size_t>(image.storage_->width) * 4U * sizeof(float);
  transfer.ptr = cpu->pixels().data();
  if (!pl_tex_download(state->gpu, &transfer)) {
    if (detect_device_loss(*state, "reading back a frame")) {
      return RenderResult<VideoFrame>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::GpuDownloadFailed,
         .message = "libplacebo could not read the GPU frame back to host memory"});
  }

  VideoFrame frame{
      .timestamp = image.storage_->timestamp,
      .duration = image.storage_->duration,
      .width = image.storage_->width,
      .height = image.storage_->height,
      .layout = PixelLayout::RgbaFloat32,
      .bit_depth = 32,
      .color = image.storage_->color,
      .field_order = "progressive",
      .sample_aspect_ratio = edit::Time(1, 1),
      .orientation_degrees = 0,
      .alpha_mode = image.storage_->alpha_mode,
      .storage = std::static_pointer_cast<const CpuFrame>(cpu),
  };
  return RenderResult<VideoFrame>::success(std::move(frame));
}

RenderResult<bool> GpuRenderer::present(const GpuImage& image) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<bool>::failure(unavailable_error(state->capabilities));
  }
  if (state->swapchain == nullptr) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "GPU renderer was created without a working presentation surface"});
  }
  if (!image.storage_ || image.storage_->texture == nullptr ||
      image.storage_->device.get() != state.get()) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuInvalidFrame,
         .message = "GPU presentation requires an image owned by this renderer device"});
  }

  const auto present_to = [&](pl_swapchain swapchain) -> RenderResult<bool> {
    if (swapchain == nullptr) {
      return RenderResult<bool>::failure(
          {.code = RenderErrorCode::GpuPresentationUnavailable,
           .message = "GPU renderer has no presentation swapchain"});
    }
    pl_swapchain_frame swapchain_frame{};
    if (!pl_swapchain_start_frame(swapchain, &swapchain_frame)) {
      if (detect_device_loss(*state, "starting a presentation frame")) {
        return RenderResult<bool>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<bool>::failure(
          {.code = RenderErrorCode::GpuPresentFailed,
           .message = "presentation surface is temporarily unavailable"});
    }
    pl_frame target{};
    pl_frame_from_swapchain(&target, &swapchain_frame);
    const pl_frame source = make_frame(image.storage_->texture, image.storage_->width,
                                       image.storage_->height);
    pl_render_params params = pl_render_default_params;
    params.upscaler = &pl_filter_bilinear;
    params.downscaler = &pl_filter_bilinear;
    const bool rendered = pl_render_image(state->renderer, &source, &target, &params);
    const bool submitted = pl_swapchain_submit_frame(swapchain);
    if (!rendered || !submitted) {
      if (detect_device_loss(*state, "presenting a frame")) {
        return RenderResult<bool>::failure(unavailable_error(state->capabilities));
      }
      return RenderResult<bool>::failure(
          {.code = RenderErrorCode::GpuPresentFailed,
           .message = rendered ? "GPU could not submit the presentation frame"
                               : "libplacebo could not render the presentation frame"});
    }
    pl_swapchain_swap_buffers(swapchain);
    return RenderResult<bool>::success(true);
  };

  state->last_secondary_present_succeeded = false;
  const auto primary = present_to(state->swapchain);
  if (!primary) {
    return primary;
  }
  if (state->secondary_swapchain != nullptr) {
    state->last_secondary_present_succeeded = static_cast<bool>(present_to(state->secondary_swapchain));
  }
  return primary;
}

RenderResult<bool> GpuRenderer::resize_presentation(const int width, const int height) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<bool>::failure(unavailable_error(state->capabilities));
  }
  if (state->swapchain == nullptr) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "GPU renderer was created without a working presentation surface"});
  }
  int resize_width = std::max(1, width);
  int resize_height = std::max(1, height);
  if (!pl_swapchain_resize(state->swapchain, &resize_width, &resize_height)) {
    if (detect_device_loss(*state, "resizing the presentation surface")) {
      return RenderResult<bool>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentFailed,
         .message = "presentation surface is temporarily unavailable"});
  }
  return RenderResult<bool>::success(true);
}

[[nodiscard]] pl_swapchain create_swapchain(LibplaceboState& state,
                                            const NativePresentationSurface& surface) {
  if (surface.surface == 0) {
    return nullptr;
  }
#if defined(_WIN32) && defined(PL_HAVE_D3D11)
  pl_d3d11_swapchain_params swapchain_params{};
  swapchain_params.window = reinterpret_cast<HWND>(surface.surface);
  swapchain_params.width = std::max(1, surface.width);
  swapchain_params.height = std::max(1, surface.height);
  return pl_d3d11_create_swapchain(state.d3d11, &swapchain_params);
#elif defined(__linux__) && defined(PL_HAVE_VULKAN)
  (void)state;
  (void)surface;
  return nullptr;
#else
  (void)state;
  (void)surface;
  return nullptr;
#endif
}

RenderResult<bool> GpuRenderer::attach_secondary_presentation(
    const NativePresentationSurface& surface) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<bool>::failure(unavailable_error(state->capabilities));
  }
  if (state->swapchain == nullptr) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "GPU renderer was created without a working presentation surface"});
  }
  if (surface.surface == 0) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "secondary presentation requires a non-zero surface handle"});
  }

  if (state->secondary_swapchain != nullptr) {
    pl_swapchain_destroy(&state->secondary_swapchain);
    state->secondary_swapchain = nullptr;
    state->capabilities.secondary_presentation = false;
  }

  state->secondary_swapchain = create_swapchain(*state, surface);
  state->capabilities.secondary_presentation = state->secondary_swapchain != nullptr;
  state->last_secondary_present_succeeded = false;
  if (state->secondary_swapchain == nullptr) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "secondary presentation swapchain creation failed"});
  }
  return RenderResult<bool>::success(true);
}

void GpuRenderer::detach_secondary_presentation() {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return;
  }
  std::scoped_lock lock(state->mutex);
  if (state->secondary_swapchain != nullptr) {
    pl_swapchain_destroy(&state->secondary_swapchain);
    state->secondary_swapchain = nullptr;
  }
  state->capabilities.secondary_presentation = false;
  state->last_secondary_present_succeeded = false;
}

RenderResult<bool> GpuRenderer::resize_secondary_presentation(const int width, const int height) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuUnavailable, .message = "GPU renderer has no state"});
  }
  std::scoped_lock lock(state->mutex);
  if (!state->capabilities.available()) {
    return RenderResult<bool>::failure(unavailable_error(state->capabilities));
  }
  if (state->secondary_swapchain == nullptr) {
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentationUnavailable,
         .message = "GPU renderer has no secondary presentation surface"});
  }
  int resize_width = std::max(1, width);
  int resize_height = std::max(1, height);
  if (!pl_swapchain_resize(state->secondary_swapchain, &resize_width, &resize_height)) {
    if (detect_device_loss(*state, "resizing the secondary presentation surface")) {
      return RenderResult<bool>::failure(unavailable_error(state->capabilities));
    }
    return RenderResult<bool>::failure(
        {.code = RenderErrorCode::GpuPresentFailed,
         .message = "secondary presentation surface is temporarily unavailable"});
  }
  return RenderResult<bool>::success(true);
}

bool GpuRenderer::secondary_presentation_attached() const noexcept {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return false;
  }
  std::scoped_lock lock(state->mutex);
  return state->secondary_swapchain != nullptr;
}

bool GpuRenderer::last_secondary_present_succeeded() const noexcept {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return false;
  }
  std::scoped_lock lock(state->mutex);
  return state->last_secondary_present_succeeded;
}

void GpuRenderer::notify_device_lost(std::string diagnostic) {
  const auto state = implementation_ ? implementation_->state : nullptr;
  if (!state) {
    return;
  }
  std::scoped_lock lock(state->mutex);
  mark_device_lost(*state, diagnostic.empty() ? "GPU device loss was reported by the platform"
                                              : std::move(diagnostic));
}

#else

struct GpuImage::Storage final {
  int width{0};
  int height{0};
  edit::Time timestamp{};
  edit::Time duration{};
  FrameColor color{};
  AlphaMode alpha_mode{AlphaMode::Premultiplied};
};

struct GpuRenderer::Impl final {
  explicit Impl(GpuCapabilities capabilities_value) : capabilities(std::move(capabilities_value)) {}

  mutable std::mutex mutex;
  GpuCapabilities capabilities;
};

GpuImage::GpuImage(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}
bool GpuImage::valid() const noexcept {
  return false;
}
int GpuImage::width() const noexcept {
  return storage_ ? storage_->width : 0;
}
int GpuImage::height() const noexcept {
  return storage_ ? storage_->height : 0;
}
edit::Time GpuImage::timestamp() const noexcept {
  return storage_ ? storage_->timestamp : edit::Time{};
}
edit::Time GpuImage::duration() const noexcept {
  return storage_ ? storage_->duration : edit::Time{};
}
const FrameColor& GpuImage::color() const noexcept {
  static const FrameColor empty{};
  return storage_ ? storage_->color : empty;
}
AlphaMode GpuImage::alpha_mode() const noexcept {
  return storage_ ? storage_->alpha_mode : AlphaMode::Premultiplied;
}

GpuRenderer::GpuRenderer(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
GpuRenderer::~GpuRenderer() = default;
GpuRenderer::GpuRenderer(GpuRenderer&&) noexcept = default;
GpuRenderer& GpuRenderer::operator=(GpuRenderer&&) noexcept = default;

std::unique_ptr<GpuRenderer> GpuRenderer::create(const GpuOptions& options) {
  const auto selection = select_gpu_backend(gpu_build_capabilities(), options.preferred_backend);
  GpuCapabilities capabilities;
  capabilities.backend = selection.backend;
  capabilities.state = GpuRuntimeState::Unavailable;
  capabilities.diagnostic = selection.diagnostic;
  return std::unique_ptr<GpuRenderer>(
      new GpuRenderer(std::make_unique<Impl>(std::move(capabilities))));
}

GpuCapabilities GpuRenderer::capabilities() const {
  if (implementation_) {
    std::scoped_lock lock(implementation_->mutex);
    return implementation_->capabilities;
  }
  GpuCapabilities missing;
  missing.diagnostic = "GPU renderer has no implementation";
  return missing;
}

RenderResult<GpuImage> GpuRenderer::upload(const VideoFrame&) {
  return RenderResult<GpuImage>::failure(unavailable_error(capabilities()));
}
RenderResult<GpuImage> GpuRenderer::apply_color_grade(const GpuImage&, const GpuColorGrade&) {
  return RenderResult<GpuImage>::failure(unavailable_error(capabilities()));
}
RenderResult<GpuImage> GpuRenderer::composite(std::span<const GpuImage>, int, int, edit::Time,
                                              edit::Time) {
  return RenderResult<GpuImage>::failure(unavailable_error(capabilities()));
}
RenderResult<GpuImage> GpuRenderer::composite_timeline(std::span<const GpuLayer>, int, int,
                                                       std::uint32_t, std::uint32_t, edit::Time,
                                                       edit::Time, const GpuImage*) {
  return RenderResult<GpuImage>::failure(unavailable_error(capabilities()));
}
RenderResult<VideoFrame> GpuRenderer::download(const GpuImage&) {
  return RenderResult<VideoFrame>::failure(unavailable_error(capabilities()));
}
RenderResult<bool> GpuRenderer::present(const GpuImage&) {
  return RenderResult<bool>::failure(unavailable_error(capabilities()));
}
RenderResult<bool> GpuRenderer::resize_presentation(int, int) {
  return RenderResult<bool>::failure(
      {.code = RenderErrorCode::GpuPresentationUnavailable,
       .message = "GPU renderer was created without a working presentation surface"});
}
RenderResult<bool> GpuRenderer::attach_secondary_presentation(const NativePresentationSurface&) {
  return RenderResult<bool>::failure(
      {.code = RenderErrorCode::GpuPresentationUnavailable,
       .message = "GPU renderer was created without a working presentation surface"});
}
void GpuRenderer::detach_secondary_presentation() {}
RenderResult<bool> GpuRenderer::resize_secondary_presentation(int, int) {
  return RenderResult<bool>::failure(
      {.code = RenderErrorCode::GpuPresentationUnavailable,
       .message = "GPU renderer has no secondary presentation surface"});
}
bool GpuRenderer::secondary_presentation_attached() const noexcept {
  return false;
}
bool GpuRenderer::last_secondary_present_succeeded() const noexcept {
  return false;
}
void GpuRenderer::notify_device_lost(std::string diagnostic) {
  if (!implementation_) {
    return;
  }
  std::scoped_lock lock(implementation_->mutex);
  implementation_->capabilities.state = GpuRuntimeState::DeviceLost;
  implementation_->capabilities.diagnostic =
      diagnostic.empty() ? "GPU device loss was reported by the platform" : std::move(diagnostic);
}

#endif

} // namespace video_editor::render

// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/gpu_backend.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

namespace video_editor::render {
namespace {

class FrameMapProvider final : public FrameProvider {
public:
  std::map<edit::EntityId, std::shared_ptr<const CpuFrame>> frames;

  RenderResult<std::shared_ptr<const CpuFrame>> request(const AssetFrameRequest& request) override {
    const auto found = frames.find(request.asset_id);
    if (found == frames.end()) {
      return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
          {.code = RenderErrorCode::AssetUnavailable, .message = "missing GPU parity fixture"});
    }
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(found->second);
  }
};

void set_premultiplied(CpuFrame& frame, const std::array<float, 4>& color) {
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      auto pixel = frame.pixel(x, y);
      pixel[0] = color[0] * color[3];
      pixel[1] = color[1] * color[3];
      pixel[2] = color[2] * color[3];
      pixel[3] = color[3];
    }
  }
}

VideoFrame video_frame(const std::shared_ptr<const CpuFrame>& pixels) {
  return {
      .timestamp = edit::Time(0, 30),
      .duration = edit::Time(1, 30),
      .width = pixels->width(),
      .height = pixels->height(),
      .layout = PixelLayout::RgbaFloat32,
      .bit_depth = 32,
      .color = {},
      .field_order = "progressive",
      .sample_aspect_ratio = edit::Time(1, 1),
      .orientation_degrees = 0,
      .alpha_mode = AlphaMode::Premultiplied,
      .storage = pixels,
  };
}

edit::TimelineSnapshot parity_snapshot(const edit::EntityId bottom_asset,
                                       const edit::EntityId top_asset) {
  edit::Project project;
  edit::Asset bottom_media;
  bottom_media.id = bottom_asset;
  bottom_media.name = "GPU parity bottom";
  bottom_media.source_uri = "memory://gpu-parity-bottom";
  bottom_media.duration = edit::Time(1, 1);
  bottom_media.has_video = true;
  bottom_media.width = 2;
  bottom_media.height = 2;
  edit::Asset top_media = bottom_media;
  top_media.id = top_asset;
  top_media.name = "GPU parity top";
  top_media.source_uri = "memory://gpu-parity-top";
  project.assets = {bottom_media, top_media};

  edit::Sequence sequence;
  sequence.width = 2;
  sequence.height = 2;
  sequence.frame_rate = edit::Rate(30, 1);

  edit::Track bottom;
  bottom.kind = edit::TrackKind::Video;
  edit::Clip bottom_clip;
  bottom_clip.asset_id = bottom_asset;
  bottom_clip.timeline_range = {edit::Time{}, edit::Time(1, 1)};
  bottom_clip.source_range = bottom_clip.timeline_range;
  bottom.clips.push_back(bottom_clip);

  edit::Track top;
  top.kind = edit::TrackKind::Video;
  edit::Clip top_clip = bottom_clip;
  top_clip.id = edit::EntityId::generate();
  top_clip.asset_id = top_asset;
  top.clips.push_back(top_clip);
  sequence.tracks = {bottom, top};
  const edit::EntityId sequence_id = sequence.id;
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value();
}

TEST(GpuBackendSelection, SelectsInjectedD3D11BuildWithoutPlatformClaims) {
  const GpuBuildCapabilities d3d_build{
      .libplacebo = true,
      .d3d11 = true,
      .vulkan = false,
      .native_default = GpuBackendKind::D3D11,
      .libplacebo_version = "test",
  };
  const auto automatic = select_gpu_backend(d3d_build, GpuBackendKind::Auto);
  EXPECT_TRUE(automatic.supported);
  EXPECT_EQ(automatic.backend, GpuBackendKind::D3D11);

  const auto unavailable = select_gpu_backend(d3d_build, GpuBackendKind::Vulkan);
  EXPECT_FALSE(unavailable.supported);
  EXPECT_EQ(unavailable.backend, GpuBackendKind::Vulkan);
  EXPECT_FALSE(unavailable.diagnostic.empty());
}

TEST(GpuBackend, BuildCapabilitiesAreTruthfulForHostPlatform) {
  const GpuBuildCapabilities build = gpu_build_capabilities();
  if (!build.libplacebo) {
    EXPECT_FALSE(build.d3d11);
    EXPECT_FALSE(build.vulkan);
    EXPECT_TRUE(build.libplacebo_version.empty());
    return;
  }
#ifdef _WIN32
  EXPECT_FALSE(build.vulkan);
  EXPECT_EQ(build.native_default, build.d3d11 ? GpuBackendKind::D3D11 : GpuBackendKind::Auto);
#elif defined(__linux__)
  EXPECT_FALSE(build.d3d11);
  EXPECT_EQ(build.native_default, build.vulkan ? GpuBackendKind::Vulkan : GpuBackendKind::Auto);
#else
  EXPECT_FALSE(build.d3d11);
  EXPECT_FALSE(build.vulkan);
#endif
  EXPECT_FALSE(build.libplacebo_version.empty());
}

TEST(GpuBackend, VulkanCompositeReadbackMatchesCpuReferenceWhenDeviceIsAvailable) {
  auto gpu =
      GpuRenderer::create({.preferred_backend = GpuBackendKind::Auto, .allow_software = true});
  ASSERT_NE(gpu, nullptr);
  const GpuCapabilities capabilities = gpu->capabilities();
  if (!capabilities.available()) {
    GTEST_SKIP() << capabilities.diagnostic;
  }
#ifdef __linux__
  ASSERT_EQ(capabilities.backend, GpuBackendKind::Vulkan);
#endif

  const auto bottom_asset = edit::EntityId::generate();
  const auto top_asset = edit::EntityId::generate();
  auto bottom = std::make_shared<CpuFrame>(2, 2);
  auto top = std::make_shared<CpuFrame>(2, 2);
  set_premultiplied(*bottom, {1.0F, 0.0F, 0.0F, 1.0F});
  set_premultiplied(*top, {0.0F, 0.0F, 1.0F, 0.5F});

  auto provider = std::make_shared<FrameMapProvider>();
  provider->frames[bottom_asset] = bottom;
  provider->frames[top_asset] = top;
  CpuRenderer cpu(provider);
  cpu.begin_epoch(1);
  const auto cpu_result =
      cpu.request_frame(parity_snapshot(bottom_asset, top_asset), edit::Time{}, {}, 1);
  ASSERT_TRUE(cpu_result) << cpu_result.error->message;
  const auto cpu_frame = std::get<std::shared_ptr<const CpuFrame>>(cpu_result.value->storage);

  const auto uploaded_bottom = gpu->upload(video_frame(bottom));
  const auto uploaded_top = gpu->upload(video_frame(top));
  ASSERT_TRUE(uploaded_bottom) << uploaded_bottom.error->message;
  ASSERT_TRUE(uploaded_top) << uploaded_top.error->message;
  const std::array<GpuImage, 2> layers{*uploaded_bottom.value, *uploaded_top.value};
  const auto composited = gpu->composite(layers, 2, 2, edit::Time{}, edit::Time(1, 30));
  ASSERT_TRUE(composited) << composited.error->message;
  const auto downloaded = gpu->download(*composited.value);
  ASSERT_TRUE(downloaded) << downloaded.error->message;
  const auto gpu_frame = std::get<std::shared_ptr<const CpuFrame>>(downloaded.value->storage);

  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const auto expected = cpu_frame->pixel(x, y);
      const auto actual = gpu_frame->pixel(x, y);
      for (std::size_t channel = 0; channel < 4U; ++channel) {
        EXPECT_NEAR(actual[channel], expected[channel], 0.0005F)
            << "pixel " << x << ',' << y << " channel " << channel;
      }
    }
  }

  const auto presentation = gpu->present(*composited.value);
  ASSERT_FALSE(presentation);
  EXPECT_EQ(presentation.error->code, RenderErrorCode::GpuPresentationUnavailable);
}

TEST(GpuBackend, DeviceLossLatchesCpuFallbackState) {
  auto gpu = GpuRenderer::create({.allow_software = true});
  ASSERT_NE(gpu, nullptr);
  gpu->notify_device_lost("injected device loss for deterministic test");
  const GpuCapabilities capabilities = gpu->capabilities();
  EXPECT_EQ(capabilities.state, GpuRuntimeState::DeviceLost);
  EXPECT_FALSE(capabilities.offscreen_rendering);
  EXPECT_FALSE(capabilities.presentation);

  auto frame = std::make_shared<CpuFrame>(1, 1);
  set_premultiplied(*frame, {1.0F, 1.0F, 1.0F, 1.0F});
  const auto result = gpu->upload(video_frame(frame));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::GpuDeviceLost);
  EXPECT_NE(result.error->message.find("injected device loss"), std::string::npos);
}

TEST(GpuBackend, CapabilityReadsAreSafeWhileDeviceLossIsReported) {
  auto gpu = GpuRenderer::create({.allow_software = true});
  ASSERT_NE(gpu, nullptr);

  std::atomic<std::uint64_t> read_count{0};
  std::atomic<bool> observed_invalid_state{false};
  std::jthread reader([&gpu, &read_count, &observed_invalid_state](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      const GpuCapabilities current = gpu->capabilities();
      if (current.state != GpuRuntimeState::Unavailable &&
          current.state != GpuRuntimeState::Ready && current.state != GpuRuntimeState::DeviceLost) {
        observed_invalid_state.store(true, std::memory_order_relaxed);
      }
      read_count.fetch_add(1U, std::memory_order_relaxed);
    }
  });

  for (std::uint64_t index = 0; index < 1'000; ++index) {
    gpu->notify_device_lost("concurrent injected loss " + std::to_string(index));
  }
  reader.request_stop();
  reader.join();

  EXPECT_GT(read_count.load(std::memory_order_relaxed), 0U);
  EXPECT_FALSE(observed_invalid_state.load(std::memory_order_relaxed));
  const GpuCapabilities final = gpu->capabilities();
  EXPECT_EQ(final.state, GpuRuntimeState::DeviceLost);
  EXPECT_EQ(final.diagnostic, "concurrent injected loss 999");
}

} // namespace
} // namespace video_editor::render

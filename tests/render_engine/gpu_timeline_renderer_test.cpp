// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/gpu_timeline_renderer.h"

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <memory>
#include <variant>
#include <vector>

namespace video_editor::render {
namespace {

class RecordingProvider final : public FrameProvider {
public:
  std::map<edit::EntityId, std::shared_ptr<const CpuFrame>> frames;
  std::vector<AssetFrameRequest> requests;

  RenderResult<std::shared_ptr<const CpuFrame>> request(const AssetFrameRequest& request) override {
    requests.push_back(request);
    const auto found = frames.find(request.asset_id);
    if (found == frames.end()) {
      return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
          {.code = RenderErrorCode::AssetUnavailable, .message = "missing GPU timeline fixture"});
    }
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(found->second);
  }
};

struct SnapshotFixture final {
  edit::TimelineSnapshot snapshot;
  edit::EntityId asset_id;
};

[[nodiscard]] SnapshotFixture make_snapshot(const edit::Transform& transform = {},
                                            const edit::BlendMode blend = edit::BlendMode::Normal,
                                            const bool muted = false, const bool visible = true,
                                            std::vector<edit::Effect> effects = {}) {
  edit::Project project;
  edit::Asset asset;
  asset.id = edit::EntityId::generate();
  asset.name = "GPU timeline fixture";
  asset.source_uri = "memory://gpu-timeline";
  asset.duration = edit::Time(10, 1);
  asset.has_video = true;
  asset.width = 8;
  asset.height = 8;
  project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.width = 8;
  sequence.height = 8;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  track.muted = muted;
  track.visible = visible;
  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.kind = edit::ClipKind::Video;
  clip.timeline_range = {edit::Time(2, 1), edit::Time(4, 1)};
  clip.source_range = {edit::Time(5, 1), edit::Time(4, 1)};
  clip.playback_rate = edit::Rate(2, 1);
  clip.transform = transform;
  clip.blend_mode = blend;
  clip.effects = std::move(effects);
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  const edit::EntityId sequence_id = sequence.id;
  project.sequences.push_back(sequence);

  edit::TimelineEditor editor(project);
  auto result = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return {.snapshot = result.value(), .asset_id = asset.id};
}

[[nodiscard]] SnapshotFixture make_title_snapshot() {
  edit::Project project;
  edit::Sequence sequence;
  sequence.width = 8;
  sequence.height = 8;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip clip;
  clip.kind = edit::ClipKind::Title;
  clip.name = "GPU Title";
  clip.timeline_range = {edit::Time(2, 1), edit::Time(4, 1)};
  clip.source_range = {edit::Time{}, edit::Time(4, 1)};
  clip.title = edit::Title{.text = "GPU"};
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  const auto sequence_id = sequence.id;
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto result = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return {.snapshot = result.value(), .asset_id = {}};
}

[[nodiscard]] SnapshotFixture make_transition_snapshot(
    const edit::TransitionKind kind = edit::TransitionKind::CrossDissolve) {
  edit::Project project;
  edit::Asset outgoing;
  outgoing.id = edit::EntityId::generate();
  outgoing.name = "Outgoing";
  outgoing.source_uri = "memory://gpu-transition-outgoing";
  outgoing.duration = edit::Time(40, 1);
  outgoing.has_video = true;
  outgoing.width = 8;
  outgoing.height = 8;
  edit::Asset incoming = outgoing;
  incoming.id = edit::EntityId::generate();
  incoming.name = "Incoming";
  incoming.source_uri = "memory://gpu-transition-incoming";
  project.assets = {outgoing, incoming};

  edit::Sequence sequence;
  sequence.width = 8;
  sequence.height = 8;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip left;
  left.asset_id = outgoing.id;
  left.timeline_range = {edit::Time(0, 1), edit::Time(10, 1)};
  left.source_range = {edit::Time(10, 1), edit::Time(10, 1)};
  edit::Clip right = left;
  right.id = edit::EntityId::generate();
  right.asset_id = incoming.id;
  right.timeline_range = {edit::Time(10, 1), edit::Time(10, 1)};
  right.source_range = {edit::Time(10, 1), edit::Time(10, 1)};
  track.clips = {left, right};
  sequence.tracks.push_back(track);

  sequence.transitions.push_back(edit::Transition{
      .outgoing_clip_id = left.id,
      .incoming_clip_id = right.id,
      .range = {edit::Time(8, 1), edit::Time(4, 1)},
      .kind = kind,
      .enabled = true,
  });

  const auto sequence_id = sequence.id;
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto result = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return SnapshotFixture{.snapshot = result.value(), .asset_id = outgoing.id};
}

[[nodiscard]] std::shared_ptr<CpuFrame> pattern() {
  auto frame = std::make_shared<CpuFrame>(8, 8);
  for (int y = 0; y < frame->height(); ++y) {
    for (int x = 0; x < frame->width(); ++x) {
      const float alpha = (x == 0 || y == 0 || x == 7 || y == 7) ? 0.0F : 1.0F;
      auto pixel = frame->pixel(x, y);
      pixel[0] = (static_cast<float>(x) / 7.0F) * alpha;
      pixel[1] = (static_cast<float>(y) / 7.0F) * alpha;
      pixel[2] = (static_cast<float>(x + y) / 14.0F) * alpha;
      pixel[3] = alpha;
    }
  }
  return frame;
}

void expect_cpu_parity(const VideoFrame& cpu_video, const VideoFrame& gpu_video,
                       const float tolerance) {
  const auto cpu = std::get<std::shared_ptr<const CpuFrame>>(cpu_video.storage);
  const auto gpu = std::get<std::shared_ptr<const CpuFrame>>(gpu_video.storage);
  ASSERT_EQ(cpu->width(), gpu->width());
  ASSERT_EQ(cpu->height(), gpu->height());
  for (int y = 0; y < cpu->height(); ++y) {
    for (int x = 0; x < cpu->width(); ++x) {
      const auto expected = cpu->pixel(x, y);
      const auto actual = gpu->pixel(x, y);
      for (std::size_t channel = 0; channel < 4U; ++channel) {
        EXPECT_NEAR(actual[channel], expected[channel], tolerance)
            << "pixel " << x << ',' << y << " channel " << channel;
      }
    }
  }
}

TEST(GpuTimelineRenderer, DecodesActiveClipAndMatchesCpuOpacityComposition) {
  edit::Transform transform;
  transform.opacity = 0.5;
  const auto fixture = make_snapshot(transform);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(
      GpuRenderer::create({.preferred_backend = GpuBackendKind::Auto, .allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }

  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(4);
  timeline.begin_epoch(4);
  const edit::Time requested_time(5, 2);
  const auto expected = cpu.request_frame(fixture.snapshot, requested_time, {}, 4);
  ASSERT_TRUE(expected) << expected.error->message;
  provider->requests.clear();
  const auto gpu_image = timeline.request_frame(fixture.snapshot, requested_time, {}, 4);
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  ASSERT_EQ(provider->requests.size(), 1U);
  EXPECT_EQ(provider->requests.front().source_time, edit::Time(6, 1));
  EXPECT_EQ(provider->requests.front().request_epoch, 4U);
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.002F);
}

TEST(GpuTimelineRenderer, QuarterTurnMatchesCpuReference) {
  edit::Transform transform;
  transform.rotation_degrees = 90.0;
  const auto fixture = make_snapshot(transform);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }

  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(9);
  timeline.begin_epoch(9);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 9);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 9);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.035F);
}

TEST(GpuTimelineRenderer, PixelAlignedCropPositionAndScaleMatchCpuReference) {
  edit::Transform transform;
  transform.position = {1.0, -1.0};
  transform.scale = {-1.0, 1.0};
  transform.crop_left = 0.125;
  transform.crop_bottom = 0.125;
  const auto fixture = make_snapshot(transform);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }

  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(10);
  timeline.begin_epoch(10);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 10);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 10);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, MutedOrEmptyTimelineReturnsOpaqueBlackWithoutDecode) {
  const auto fixture = make_snapshot({}, edit::BlendMode::Normal, true);
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  GpuTimelineRenderer timeline(provider, gpu);
  timeline.begin_epoch(1);
  const auto image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 1);
  ASSERT_TRUE(image) << image.error->message;
  EXPECT_TRUE(provider->requests.empty());
  const auto downloaded = gpu->download(*image.value);
  ASSERT_TRUE(downloaded) << downloaded.error->message;
  const auto pixels = std::get<std::shared_ptr<const CpuFrame>>(downloaded.value->storage);
  const auto first = pixels->pixel(0, 0);
  EXPECT_FLOAT_EQ(first[0], 0.0F);
  EXPECT_FLOAT_EQ(first[1], 0.0F);
  EXPECT_FLOAT_EQ(first[2], 0.0F);
  EXPECT_FLOAT_EQ(first[3], 1.0F);
}

TEST(GpuTimelineRenderer, InvisibleTimelineReturnsOpaqueBlackWithoutDecode) {
  const auto fixture = make_snapshot({}, edit::BlendMode::Normal, false, false);
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  GpuTimelineRenderer renderer(provider, gpu);
  renderer.begin_epoch(1);
  const auto result = renderer.request_frame(fixture.snapshot, edit::Time(3, 1), {}, 1);
  ASSERT_TRUE(result) << result.error->message;
  EXPECT_TRUE(provider->requests.empty());
  const auto downloaded = gpu->download(*result.value);
  ASSERT_TRUE(downloaded) << downloaded.error->message;
  const auto pixels = std::get<std::shared_ptr<const CpuFrame>>(downloaded.value->storage);
  EXPECT_FLOAT_EQ(pixels->pixel(0, 0)[0], 0.0F);
  EXPECT_FLOAT_EQ(pixels->pixel(0, 0)[3], 1.0F);
}

TEST(GpuTimelineRenderer, OverlayBlendMatchesCpuReference) {
  const auto fixture = make_snapshot({}, edit::BlendMode::Overlay);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(2);
  timeline.begin_epoch(2);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 2);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 2);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, MultiplyBlendMatchesCpuReference) {
  const auto fixture = make_snapshot({}, edit::BlendMode::Multiply);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(13);
  timeline.begin_epoch(13);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 13);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 13);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, RotationWithMovedPivotMatchesCpuReference) {
  edit::Transform transform;
  transform.rotation_degrees = 30.0;
  transform.position.x = 1.0;
  transform.anchor_x = 0.25;
  const auto fixture = make_snapshot(transform);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(3);
  timeline.begin_epoch(3);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 3);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 3);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.05F);
}

TEST(GpuTimelineRenderer, ActiveTitleMatchesCpuReference) {
  const auto fixture = make_title_snapshot();
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(11);
  timeline.begin_epoch(11);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 11);
  provider->requests.clear();
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 11);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  EXPECT_TRUE(provider->requests.empty());
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, CrossDissolveTransitionMatchesCpuReference) {
  const auto fixture = make_transition_snapshot();
  auto provider = std::make_shared<RecordingProvider>();
  for (const edit::Clip& clip : fixture.snapshot.sequence().tracks.front().clips) {
    provider->frames[clip.asset_id] = pattern();
  }
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(12);
  timeline.begin_epoch(12);
  const edit::Time requested_time(10, 1);
  const auto expected = cpu.request_frame(fixture.snapshot, requested_time, {}, 12);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, requested_time, {}, 12);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, DipToBlackBeforeCutMatchesCpuReference) {
  const auto fixture = make_transition_snapshot(edit::TransitionKind::DipToBlack);
  auto provider = std::make_shared<RecordingProvider>();
  for (const edit::Clip& clip : fixture.snapshot.sequence().tracks.front().clips) {
    provider->frames[clip.asset_id] = pattern();
  }
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(14);
  timeline.begin_epoch(14);
  const edit::Time requested_time(9, 1);
  const auto expected = cpu.request_frame(fixture.snapshot, requested_time, {}, 14);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, requested_time, {}, 14);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, ColorEffectMatchesCpuReference) {
  edit::Effect color_effect;
  color_effect.enabled = true;
  color_effect.known = true;
  color_effect.type = "video.color";
  edit::EffectParameter exposure{.id = "exposure", .value = 0.5, .keyframes = {}};
  color_effect.parameters.emplace(exposure.id, exposure);
  const auto fixture = make_snapshot({}, edit::BlendMode::Normal, false, true, {color_effect});
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[fixture.asset_id] = pattern();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(15);
  timeline.begin_epoch(15);
  const auto expected = cpu.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 15);
  const auto gpu_image = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 15);
  ASSERT_TRUE(expected) << expected.error->message;
  ASSERT_TRUE(gpu_image) << gpu_image.error->message;
  const auto actual = gpu->download(*gpu_image.value);
  ASSERT_TRUE(actual) << actual.error->message;
  expect_cpu_parity(*expected.value, *actual.value, 0.003F);
}

TEST(GpuTimelineRenderer, UnknownEnabledEffectReturnsTypedFallbackBeforeDecode) {
  edit::Effect unknown_effect;
  unknown_effect.enabled = true;
  unknown_effect.known = true;
  unknown_effect.type = "video.custom_filter";
  const auto fixture = make_snapshot({}, edit::BlendMode::Normal, false, true, {unknown_effect});
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  GpuTimelineRenderer timeline(provider, gpu);
  timeline.begin_epoch(16);
  const auto result = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 16);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::GpuUnsupportedTimeline);
  EXPECT_TRUE(provider->requests.empty());
  timeline.begin_epoch(17);
  const auto plain_fixture = make_snapshot({}, edit::BlendMode::Normal, false, true, {});
  provider->frames[plain_fixture.asset_id] = pattern();
  const auto recovered =
      timeline.request_frame(plain_fixture.snapshot, edit::Time(5, 2), {}, 17);
  if (gpu->capabilities().available()) {
    ASSERT_TRUE(recovered) << recovered.error->message;
  }
}

TEST(GpuTimelineRenderer, RejectsStaleEpochBeforeDecode) {
  const auto fixture = make_snapshot();
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  GpuTimelineRenderer timeline(provider, gpu);
  timeline.begin_epoch(8);
  const auto result = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 7);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::StaleRequest);
  EXPECT_TRUE(provider->requests.empty());
}

TEST(GpuTimelineRenderer, DeviceLostKeepsCpuFrameAndLeavesRevisionUnchanged) {
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  ASSERT_NE(gpu, nullptr);
  if (!gpu->capabilities().available()) {
    GTEST_SKIP() << gpu->capabilities().diagnostic;
  }

  edit::Project project;
  edit::Asset asset;
  asset.id = edit::EntityId::generate();
  asset.name = "device-lost fixture";
  asset.source_uri = "memory://device-lost";
  asset.duration = edit::Time(10, 1);
  asset.has_video = true;
  asset.width = 8;
  asset.height = 8;
  project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.width = 8;
  sequence.height = 8;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.kind = edit::ClipKind::Video;
  clip.timeline_range = {edit::Time(2, 1), edit::Time(4, 1)};
  clip.source_range = {edit::Time(5, 1), edit::Time(4, 1)};
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  const edit::EntityId sequence_id = sequence.id;
  project.sequences.push_back(sequence);

  edit::TimelineEditor editor(project);
  const auto revision = editor.revision();
  auto snapshot = editor.snapshot(sequence_id, revision);
  ASSERT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);

  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[asset.id] = pattern();
  CpuRenderer cpu(provider);
  GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(3);
  timeline.begin_epoch(3);
  const edit::Time requested_time(5, 2);
  const auto cpu_before = cpu.request_frame(snapshot.value(), requested_time, {}, 3);
  ASSERT_TRUE(cpu_before) << cpu_before.error->message;
  const auto* cpu_storage =
      std::get_if<std::shared_ptr<const CpuFrame>>(&cpu_before.value->storage);
  ASSERT_NE(cpu_storage, nullptr);
  ASSERT_TRUE(*cpu_storage);

  gpu->notify_device_lost("injected device loss for preview fallback");
  EXPECT_EQ(gpu->capabilities().state, GpuRuntimeState::DeviceLost);
  const auto gpu_after = timeline.request_frame(snapshot.value(), requested_time, {}, 3);
  ASSERT_FALSE(gpu_after);
  EXPECT_EQ(gpu_after.error->code, RenderErrorCode::GpuDeviceLost);

  cpu.begin_epoch(4);
  const auto cpu_after = cpu.request_frame(snapshot.value(), requested_time, {}, 4);
  ASSERT_TRUE(cpu_after) << cpu_after.error->message;
  const auto* cpu_after_storage =
      std::get_if<std::shared_ptr<const CpuFrame>>(&cpu_after.value->storage);
  ASSERT_NE(cpu_after_storage, nullptr);
  ASSERT_TRUE(*cpu_after_storage);
  EXPECT_EQ((*cpu_after_storage)->width(), (*cpu_storage)->width());
  EXPECT_EQ((*cpu_after_storage)->height(), (*cpu_storage)->height());
  EXPECT_EQ(editor.revision(), revision);
}

TEST(GpuTimelineRenderer, EnabledLutReturnsGpuUnsupportedTimeline) {
  edit::Effect lut_effect;
  lut_effect.enabled = true;
  lut_effect.known = true;
  lut_effect.type = "video.lut";
  lut_effect.parameters.emplace("path", edit::EffectParameter{.id = "path",
                                                            .value = std::string{"/tmp/test.cube"},
                                                            .keyframes = {}});
  const auto fixture = make_snapshot({}, edit::BlendMode::Normal, false, true, {lut_effect});
  auto provider = std::make_shared<RecordingProvider>();
  auto gpu = std::shared_ptr<GpuRenderer>(GpuRenderer::create({.allow_software = true}));
  GpuTimelineRenderer timeline(provider, gpu);
  timeline.begin_epoch(18);
  const auto result = timeline.request_frame(fixture.snapshot, edit::Time(5, 2), {}, 18);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::GpuUnsupportedTimeline);
  EXPECT_TRUE(provider->requests.empty());
}

} // namespace
} // namespace video_editor::render

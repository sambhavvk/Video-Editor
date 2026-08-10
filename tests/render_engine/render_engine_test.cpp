// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/render_cache.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

namespace video_editor::render {
namespace {

class SolidProvider final : public FrameProvider {
public:
  std::map<edit::EntityId, std::array<float, 4>> colors;
  std::optional<AssetFrameRequest> last_request;

  RenderResult<std::shared_ptr<const CpuFrame>> request(const AssetFrameRequest& request) override {
    last_request = request;
    const auto iterator = colors.find(request.asset_id);
    if (iterator == colors.end()) {
      return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
          {.code = RenderErrorCode::AssetUnavailable, .message = "missing test asset"});
    }
    auto frame = std::make_shared<CpuFrame>(2, 2);
    frame->clear(iterator->second[0], iterator->second[1], iterator->second[2],
                 iterator->second[3]);
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(frame);
  }
};

edit::TimelineSnapshot make_snapshot(edit::EntityId& bottom_asset, edit::EntityId& top_asset,
                                     const edit::BlendMode top_blend = edit::BlendMode::Normal) {
  edit::Project project;
  edit::Sequence sequence;
  sequence.width = 2;
  sequence.height = 2;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track bottom;
  bottom.kind = edit::TrackKind::Video;
  edit::Clip red;
  bottom_asset = red.asset_id = edit::EntityId::generate();
  red.timeline_range = {edit::Time(0, 30), edit::Time(30, 30)};
  red.source_range = red.timeline_range;
  bottom.clips.push_back(red);
  edit::Track top;
  top.kind = edit::TrackKind::Video;
  edit::Clip blue;
  top_asset = blue.asset_id = edit::EntityId::generate();
  blue.timeline_range = red.timeline_range;
  blue.source_range = red.source_range;
  blue.transform.opacity = 0.5;
  blue.blend_mode = top_blend;
  top.clips.push_back(blue);
  sequence.tracks = {bottom, top};
  const auto sequence_id = sequence.id;
  edit::Asset bottom_media;
  bottom_media.id = bottom_asset;
  bottom_media.name = "Bottom solid";
  bottom_media.source_uri = "memory://bottom";
  bottom_media.duration = edit::Time(30, 30);
  bottom_media.has_video = true;
  bottom_media.width = 2;
  bottom_media.height = 2;
  edit::Asset top_media = bottom_media;
  top_media.id = top_asset;
  top_media.name = "Top solid";
  top_media.source_uri = "memory://top";
  project.assets = {bottom_media, top_media};
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot);
  return snapshot.value();
}

class PatternProvider final : public FrameProvider {
public:
  std::map<edit::EntityId, std::shared_ptr<const CpuFrame>> frames;

  RenderResult<std::shared_ptr<const CpuFrame>> request(const AssetFrameRequest& request) override {
    const auto found = frames.find(request.asset_id);
    if (found == frames.end()) {
      return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
          {.code = RenderErrorCode::AssetUnavailable, .message = "missing test frame"});
    }
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(found->second);
  }
};

edit::TimelineSnapshot make_single_snapshot(const edit::EntityId asset_id, const int width,
                                            const int height, const edit::Transform& transform,
                                            const edit::BlendMode blend = edit::BlendMode::Normal) {
  edit::Project project;
  edit::Asset asset;
  asset.id = asset_id;
  asset.name = "Pattern";
  asset.source_uri = "memory://pattern";
  asset.duration = edit::Time(30, 30);
  asset.has_video = true;
  asset.width = static_cast<std::uint32_t>(width);
  asset.height = static_cast<std::uint32_t>(height);
  project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.width = static_cast<std::uint32_t>(width);
  sequence.height = static_cast<std::uint32_t>(height);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip clip;
  clip.asset_id = asset_id;
  clip.kind = edit::ClipKind::Video;
  clip.timeline_range = {edit::Time(0, 30), edit::Time(30, 30)};
  clip.source_range = clip.timeline_range;
  clip.transform = transform;
  clip.blend_mode = blend;
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  const auto sequence_id = sequence.id;
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value();
}

void set_pixel(CpuFrame& frame, const int x, const int y, const std::array<float, 4>& color) {
  auto pixel = frame.pixel(x, y);
  for (std::size_t channel = 0; channel < color.size(); ++channel) {
    pixel[channel] = color[channel] * (channel == 3U ? 1.0F : color[3]);
  }
}

TEST(CpuRenderer, CompositesTracksBottomToTopAndMapsExactTime) {
  edit::EntityId bottom_asset;
  edit::EntityId top_asset;
  auto snapshot = make_snapshot(bottom_asset, top_asset);
  auto provider = std::make_shared<SolidProvider>();
  provider->colors[bottom_asset] = {1.0F, 0.0F, 0.0F, 1.0F};
  provider->colors[top_asset] = {0.0F, 0.0F, 1.0F, 1.0F};
  CpuRenderer renderer(provider);
  renderer.begin_epoch(7);
  const auto result = renderer.request_frame(snapshot, edit::Time(15, 30), {}, 7);
  ASSERT_TRUE(result) << result.error->message;
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  const auto pixel = frame->pixel(0, 0);
  EXPECT_NEAR(pixel[0], 0.5F, 0.001F);
  EXPECT_NEAR(pixel[1], 0.0F, 0.001F);
  EXPECT_NEAR(pixel[2], 0.5F, 0.001F);
  ASSERT_TRUE(provider->last_request.has_value());
  EXPECT_EQ(provider->last_request->source_time, edit::Time(15, 30));
}

TEST(CpuRenderer, RejectsStaleEpochBeforeProviderWork) {
  edit::EntityId bottom_asset;
  edit::EntityId top_asset;
  auto snapshot = make_snapshot(bottom_asset, top_asset);
  auto provider = std::make_shared<SolidProvider>();
  CpuRenderer renderer(provider);
  renderer.begin_epoch(9);
  const auto result = renderer.request_frame(snapshot, edit::Time(0, 30), {}, 8);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::StaleRequest);
  EXPECT_FALSE(provider->last_request.has_value());
}

TEST(CpuRenderer, BilinearScaleUsesPixelCentersAndSignedScaleFlips) {
  const auto asset_id = edit::EntityId::generate();
  auto source = std::make_shared<CpuFrame>(2, 1);
  set_pixel(*source, 0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
  set_pixel(*source, 1, 0, {0.0F, 0.0F, 1.0F, 1.0F});
  auto provider = std::make_shared<PatternProvider>();
  provider->frames[asset_id] = source;

  edit::Transform scaled;
  scaled.scale.x = 2.0;
  const auto scaled_snapshot = make_single_snapshot(asset_id, 3, 1, scaled);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(1);
  const auto scaled_result = renderer.request_frame(scaled_snapshot, edit::Time{}, {}, 1);
  ASSERT_TRUE(scaled_result) << scaled_result.error->message;
  const auto scaled_frame = std::get<std::shared_ptr<const CpuFrame>>(scaled_result.value->storage);
  EXPECT_NEAR(scaled_frame->pixel(0, 0)[0], 1.0F, 0.0001F);
  EXPECT_NEAR(scaled_frame->pixel(1, 0)[0], 0.5F, 0.0001F);
  EXPECT_NEAR(scaled_frame->pixel(1, 0)[2], 0.5F, 0.0001F);
  EXPECT_NEAR(scaled_frame->pixel(2, 0)[2], 1.0F, 0.0001F);

  edit::Transform flipped;
  flipped.scale.x = -1.0;
  const auto flipped_snapshot = make_single_snapshot(asset_id, 2, 1, flipped);
  renderer.begin_epoch(2);
  const auto flipped_result = renderer.request_frame(flipped_snapshot, edit::Time{}, {}, 2);
  ASSERT_TRUE(flipped_result) << flipped_result.error->message;
  const auto flipped_frame =
      std::get<std::shared_ptr<const CpuFrame>>(flipped_result.value->storage);
  EXPECT_NEAR(flipped_frame->pixel(0, 0)[2], 1.0F, 0.0001F);
  EXPECT_NEAR(flipped_frame->pixel(1, 0)[0], 1.0F, 0.0001F);
}

TEST(CpuRenderer, PositionAndAnchorUseSequencePixelCoordinatesWithTransparentBounds) {
  const auto asset_id = edit::EntityId::generate();
  auto source = std::make_shared<CpuFrame>(2, 1);
  set_pixel(*source, 0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
  set_pixel(*source, 1, 0, {0.0F, 0.0F, 1.0F, 1.0F});
  auto provider = std::make_shared<PatternProvider>();
  provider->frames[asset_id] = source;

  edit::Transform anchored;
  anchored.anchor_x = 0.0;
  const auto anchored_snapshot = make_single_snapshot(asset_id, 3, 1, anchored);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(3);
  const auto anchored_result = renderer.request_frame(anchored_snapshot, edit::Time{}, {}, 3);
  ASSERT_TRUE(anchored_result) << anchored_result.error->message;
  const auto anchored_frame =
      std::get<std::shared_ptr<const CpuFrame>>(anchored_result.value->storage);
  EXPECT_NEAR(anchored_frame->pixel(0, 0)[0], 0.0F, 0.0001F);
  EXPECT_NEAR(anchored_frame->pixel(1, 0)[0], 1.0F, 0.0001F);
  EXPECT_NEAR(anchored_frame->pixel(2, 0)[2], 1.0F, 0.0001F);

  anchored.position.x = 1.0;
  const auto positioned_snapshot = make_single_snapshot(asset_id, 3, 1, anchored);
  renderer.begin_epoch(4);
  const auto positioned_result = renderer.request_frame(positioned_snapshot, edit::Time{}, {}, 4);
  ASSERT_TRUE(positioned_result) << positioned_result.error->message;
  const auto positioned_frame =
      std::get<std::shared_ptr<const CpuFrame>>(positioned_result.value->storage);
  EXPECT_NEAR(positioned_frame->pixel(1, 0)[0], 0.0F, 0.0001F);
  EXPECT_NEAR(positioned_frame->pixel(2, 0)[0], 1.0F, 0.0001F);
}

TEST(CpuRenderer, RotationUsesNormalizedAnchorAsPivot) {
  const auto asset_id = edit::EntityId::generate();
  auto source = std::make_shared<CpuFrame>(3, 3);
  source->clear(0.0F, 0.0F, 0.0F, 0.0F);
  set_pixel(*source, 0, 1, {1.0F, 0.0F, 0.0F, 1.0F});
  auto provider = std::make_shared<PatternProvider>();
  provider->frames[asset_id] = source;
  edit::Transform transform;
  transform.rotation_degrees = 90.0;
  const auto snapshot = make_single_snapshot(asset_id, 3, 3, transform);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(5);
  const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 5);
  ASSERT_TRUE(result) << result.error->message;
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  EXPECT_NEAR(frame->pixel(1, 0)[0], 1.0F, 0.0001F);
  EXPECT_NEAR(frame->pixel(0, 1)[0], 0.0F, 0.0001F);
}

TEST(CpuRenderer, CropMasksSourcePixelsBeforeBilinearSampling) {
  const auto asset_id = edit::EntityId::generate();
  auto source = std::make_shared<CpuFrame>(3, 1);
  set_pixel(*source, 0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
  set_pixel(*source, 1, 0, {0.0F, 1.0F, 0.0F, 1.0F});
  set_pixel(*source, 2, 0, {0.0F, 0.0F, 1.0F, 1.0F});
  auto provider = std::make_shared<PatternProvider>();
  provider->frames[asset_id] = source;
  edit::Transform transform;
  transform.crop_left = 1.0 / 3.0;
  transform.crop_right = 1.0 / 3.0;
  const auto snapshot = make_single_snapshot(asset_id, 3, 1, transform);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(6);
  const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 6);
  ASSERT_TRUE(result) << result.error->message;
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  EXPECT_NEAR(frame->pixel(0, 0)[0], 0.0F, 0.0001F);
  EXPECT_NEAR(frame->pixel(1, 0)[1], 1.0F, 0.0001F);
  EXPECT_NEAR(frame->pixel(2, 0)[2], 0.0F, 0.0001F);
}

TEST(CpuRenderer, BlendModesHaveDeterministicLinearPremultipliedResults) {
  struct Expectation final {
    edit::BlendMode mode;
    std::array<float, 3> color;
  };
  const std::vector<Expectation> expectations{
      {edit::BlendMode::Normal, {0.525F, 0.4F, 0.65F}},
      {edit::BlendMode::Add, {0.625F, 0.7F, 0.9F}},
      {edit::BlendMode::Multiply, {0.225F, 0.36F, 0.6F}},
      {edit::BlendMode::Screen, {0.55F, 0.64F, 0.85F}},
      {edit::BlendMode::Overlay, {0.325F, 0.48F, 0.8F}},
  };
  for (const auto& expectation : expectations) {
    edit::EntityId bottom_asset;
    edit::EntityId top_asset;
    const auto snapshot = make_snapshot(bottom_asset, top_asset, expectation.mode);
    auto provider = std::make_shared<SolidProvider>();
    provider->colors[bottom_asset] = {0.25F, 0.6F, 0.8F, 1.0F};
    provider->colors[top_asset] = {0.8F, 0.2F, 0.5F, 1.0F};
    CpuRenderer renderer(provider);
    renderer.begin_epoch(11);
    const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 11);
    ASSERT_TRUE(result) << result.error->message;
    const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
    const auto pixel = frame->pixel(0, 0);
    for (std::size_t channel = 0; channel < expectation.color.size(); ++channel) {
      EXPECT_NEAR(pixel[channel], expectation.color[channel], 0.0001F)
          << "blend mode " << static_cast<int>(expectation.mode) << " channel " << channel;
    }
    EXPECT_NEAR(pixel[3], 1.0F, 0.0001F);
  }
}

TEST(RenderCache, EvictsLeastRecentlyUsedFramesToBoundMemory) {
  RenderCache cache(2U * 2U * 4U * sizeof(float));
  auto first = std::make_shared<CpuFrame>(2, 2);
  auto second = std::make_shared<CpuFrame>(2, 2);
  const edit::EntityId sequence = edit::EntityId::generate();
  const RenderCacheKey key_a{.revision = {1},
                             .sequence_id = sequence,
                             .time = edit::Time(0, 30),
                             .width = 2,
                             .height = 2,
                             .graph_signature = 1};
  const RenderCacheKey key_b{.revision = {1},
                             .sequence_id = sequence,
                             .time = edit::Time(1, 30),
                             .width = 2,
                             .height = 2,
                             .graph_signature = 1};
  cache.put(key_a, first);
  cache.put(key_b, second);
  EXPECT_FALSE(cache.get(key_a));
  EXPECT_TRUE(cache.get(key_b));
}

} // namespace
} // namespace video_editor::render

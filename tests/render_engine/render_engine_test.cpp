// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/render_cache.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>

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

edit::TimelineSnapshot make_snapshot(edit::EntityId& bottom_asset, edit::EntityId& top_asset) {
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

TEST(RenderCache, EvictsLeastRecentlyUsedFramesToBoundMemory) {
  RenderCache cache(2U * 2U * 4U * sizeof(float));
  auto first = std::make_shared<CpuFrame>(2, 2);
  auto second = std::make_shared<CpuFrame>(2, 2);
  const edit::EntityId sequence = edit::EntityId::generate();
  const RenderCacheKey key_a{.revision = {1}, .sequence_id = sequence, .time = edit::Time(0, 30),
                             .width = 2, .height = 2, .graph_signature = 1};
  const RenderCacheKey key_b{.revision = {1}, .sequence_id = sequence, .time = edit::Time(1, 30),
                             .width = 2, .height = 2, .graph_signature = 1};
  cache.put(key_a, first);
  cache.put(key_b, second);
  EXPECT_FALSE(cache.get(key_a));
  EXPECT_TRUE(cache.get(key_b));
}

} // namespace
} // namespace video_editor::render

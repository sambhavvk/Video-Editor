// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/render_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
                                     const edit::BlendMode top_blend = edit::BlendMode::Normal,
                                     const bool top_visible = true) {
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
  top.visible = top_visible;
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

edit::TimelineSnapshot make_effect_snapshot(const edit::EntityId asset_id,
                                            std::vector<edit::Effect> effects, const int width = 2,
                                            const int height = 1) {
  edit::Project project;
  edit::Asset asset;
  asset.id = asset_id;
  asset.name = "Effect source";
  asset.source_uri = "memory://effect";
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
  clip.timeline_range = {edit::Time(0, 30), edit::Time(30, 30)};
  clip.source_range = clip.timeline_range;
  clip.effects = std::move(effects);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  const auto sequence_id = sequence.id;
  project.sequences.push_back(std::move(sequence));
  edit::TimelineEditor editor(std::move(project));
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value();
}

class RecordingProvider final : public FrameProvider {
public:
  std::map<edit::EntityId, std::shared_ptr<const CpuFrame>> frames;
  std::vector<AssetFrameRequest> requests;
  std::function<void()> on_request;

  RenderResult<std::shared_ptr<const CpuFrame>> request(const AssetFrameRequest& request) override {
    requests.push_back(request);
    if (on_request) {
      on_request();
      on_request = nullptr;
    }
    const auto found = frames.find(request.asset_id);
    if (found == frames.end()) {
      return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
          {.code = RenderErrorCode::AssetUnavailable, .message = "missing recorded frame"});
    }
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(found->second);
  }
};

void assign_title_payload(edit::Clip& clip, const std::string& text, const double font_size,
                          const bool bold = false, const bool italic = false) {
  clip.title = edit::Title{
      .text = text,
      .font_family = "Inter",
      .font_size = font_size,
      .foreground_color = {1.0, 1.0, 1.0, 1.0},
      .background_color = {0.0, 0.0, 0.0, 0.0},
      .horizontal_alignment = edit::TitleHorizontalAlignment::Center,
      .bold = bold,
      .italic = italic,
  };
}

edit::TimelineSnapshot make_title_snapshot(const std::string& text, const double font_size = 96.0,
                                           const bool bold = false, const bool italic = false,
                                           const std::uint32_t width = 24,
                                           const std::uint32_t height = 8,
                                           const std::string& clip_name = {}) {
  edit::Project project;
  edit::Sequence sequence;
  sequence.width = width;
  sequence.height = height;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip title;
  title.kind = edit::ClipKind::Title;
  title.name = clip_name.empty() ? text : clip_name;
  title.timeline_range = {edit::Time(0, 30), edit::Time(30, 30)};
  title.source_range = title.timeline_range;
  assign_title_payload(title, text, font_size, bold, italic);
  track.clips.push_back(title);
  sequence.tracks.push_back(track);
  const auto sequence_id = sequence.id;
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(project);
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value();
}

int count_foreground_pixels(const std::shared_ptr<const CpuFrame>& frame) {
  int lit_pixels = 0;
  for (int y = 0; y < frame->height(); ++y) {
    for (int x = 0; x < frame->width(); ++x) {
      const auto pixel = frame->pixel(x, y);
      if (pixel[0] > 0.01F || pixel[1] > 0.01F || pixel[2] > 0.01F) {
        ++lit_pixels;
      }
    }
  }
  return lit_pixels;
}

int title_bbox_width(const std::shared_ptr<const CpuFrame>& frame) {
  int min_x = frame->width();
  int max_x = -1;
  for (int y = 0; y < frame->height(); ++y) {
    for (int x = 0; x < frame->width(); ++x) {
      const auto pixel = frame->pixel(x, y);
      if (pixel[0] > 0.01F || pixel[1] > 0.01F || pixel[2] > 0.01F) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
      }
    }
  }
  return max_x >= min_x ? (max_x - min_x + 1) : 0;
}

edit::TimelineSnapshot make_transition_snapshot(const edit::TransitionKind kind) {
  edit::Project project;
  edit::Asset outgoing_asset;
  outgoing_asset.id = edit::EntityId::generate();
  outgoing_asset.name = "Outgoing";
  outgoing_asset.source_uri = "memory://outgoing";
  outgoing_asset.duration = edit::Time(400, 1);
  outgoing_asset.has_video = true;
  outgoing_asset.width = 2;
  outgoing_asset.height = 2;
  edit::Asset incoming_asset = outgoing_asset;
  incoming_asset.id = edit::EntityId::generate();
  incoming_asset.name = "Incoming";
  incoming_asset.source_uri = "memory://incoming";
  project.assets = {outgoing_asset, incoming_asset};

  edit::Sequence sequence;
  sequence.width = 2;
  sequence.height = 2;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip outgoing;
  outgoing.asset_id = outgoing_asset.id;
  outgoing.timeline_range = {edit::Time(0, 1), edit::Time(10, 1)};
  outgoing.source_range = {edit::Time(100, 1), edit::Time(10, 1)};
  edit::Clip incoming;
  incoming.id = edit::EntityId::generate();
  incoming.asset_id = incoming_asset.id;
  incoming.timeline_range = {edit::Time(10, 1), edit::Time(10, 1)};
  incoming.source_range = {edit::Time(200, 1), edit::Time(10, 1)};
  track.clips = {outgoing, incoming};
  sequence.tracks.push_back(track);

  sequence.transitions.push_back(edit::Transition{
      .outgoing_clip_id = outgoing.id,
      .incoming_clip_id = incoming.id,
      .range = {edit::Time(8, 1), edit::Time(4, 1)},
      .kind = kind,
      .enabled = true,
  });
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

TEST(CpuRenderer, SkipsInvisibleVideoTracks) {
  edit::EntityId bottom_asset;
  edit::EntityId top_asset;
  const auto snapshot = make_snapshot(bottom_asset, top_asset, edit::BlendMode::Normal, false);
  auto provider = std::make_shared<SolidProvider>();
  provider->colors.emplace(bottom_asset, std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
  provider->colors.emplace(top_asset, std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
  CpuRenderer renderer(provider);
  renderer.begin_epoch(1);
  const auto result = renderer.request_frame(snapshot, edit::Time(15, 30), {}, 1);
  ASSERT_TRUE(result) << result.error->message;
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  EXPECT_NEAR(frame->pixel(0, 0)[0], 1.0F, 0.0001F);
  EXPECT_NEAR(frame->pixel(0, 0)[2], 0.0F, 0.0001F);
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

TEST(CpuRenderer, EvaluatesColorKeyframesAtClipLocalTime) {
  const auto asset_id = edit::EntityId::generate();
  edit::Effect effect;
  effect.type = "video.color";
  edit::EffectParameter exposure{.id = "exposure", .value = 0.0, .keyframes = {}};
  exposure.keyframes = {
      edit::Keyframe{.time = edit::Time(0, 1), .value = 0.0},
      edit::Keyframe{.time = edit::Time(1, 2), .value = 2.0},
  };
  effect.parameters.emplace(exposure.id, exposure);
  const auto snapshot = make_effect_snapshot(asset_id, {effect});
  auto source = std::make_shared<CpuFrame>(2, 1);
  source->clear(0.25F, 0.25F, 0.25F, 1.0F);
  auto provider = std::make_shared<PatternProvider>();
  provider->frames.emplace(asset_id, source);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(1);
  const auto before = renderer.request_frame(snapshot, edit::Time(0, 1), {}, 1);
  const auto after = renderer.request_frame(snapshot, edit::Time(1, 2), {}, 1);
  ASSERT_TRUE(before);
  ASSERT_TRUE(after);
  const auto before_frame = std::get<std::shared_ptr<const CpuFrame>>(before.value->storage);
  const auto after_frame = std::get<std::shared_ptr<const CpuFrame>>(after.value->storage);
  EXPECT_NEAR(before_frame->pixel(0, 0)[0], 0.25F, 0.001F);
  EXPECT_NEAR(after_frame->pixel(0, 0)[0], 1.0F, 0.001F);
}

TEST(CpuRenderer, BypassesGaussianBlurOnlyForPreview) {
  const auto asset_id = edit::EntityId::generate();
  edit::Effect effect;
  effect.type = "video.gaussian_blur";
  effect.parameters.emplace("radius",
                            edit::EffectParameter{.id = "radius", .value = 1.0, .keyframes = {}});
  const auto snapshot = make_effect_snapshot(asset_id, {effect});
  auto source = std::make_shared<CpuFrame>(2, 1);
  set_pixel(*source, 0, 0, {1.0F, 0.0F, 0.0F, 1.0F});
  set_pixel(*source, 1, 0, {0.0F, 0.0F, 1.0F, 1.0F});
  auto provider = std::make_shared<PatternProvider>();
  provider->frames.emplace(asset_id, source);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(1);
  const auto preview = renderer.request_frame(snapshot, edit::Time(0, 1),
                                              PreviewProfile{.bypass_expensive_effects = true}, 1);
  const auto full = renderer.request_frame(snapshot, edit::Time(0, 1),
                                           PreviewProfile{.bypass_expensive_effects = false}, 1);
  ASSERT_TRUE(preview);
  ASSERT_TRUE(full);
  const auto preview_frame = std::get<std::shared_ptr<const CpuFrame>>(preview.value->storage);
  const auto full_frame = std::get<std::shared_ptr<const CpuFrame>>(full.value->storage);
  EXPECT_NEAR(preview_frame->pixel(0, 0)[0], 1.0F, 0.001F);
  EXPECT_NEAR(full_frame->pixel(0, 0)[0], 0.5F, 0.001F);
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

TEST(CpuRenderer, TitleClipUsesDeterministicReplacementGlyphWithoutProviderDecode) {
  const auto snapshot =
      make_title_snapshot(std::string("A\xF0\x9F\x99\x82"), 96.0, false, false, 24, 24);
  auto provider = std::make_shared<RecordingProvider>();
  CpuRenderer renderer(provider);
  renderer.begin_epoch(12);
  const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 12);
  ASSERT_TRUE(result) << result.error->message;
  EXPECT_TRUE(provider->requests.empty());
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  int lit_pixels = 0;
  for (int y = 0; y < frame->height(); ++y) {
    for (int x = 0; x < frame->width(); ++x) {
      if (frame->pixel(x, y)[3] > 0.0F) {
        ++lit_pixels;
      }
    }
  }
  EXPECT_GT(lit_pixels, 0);
}

TEST(CpuRenderer, TitleFontSizeBoldAndItalicAffectDeterministicRaster) {
  auto provider = std::make_shared<RecordingProvider>();
  CpuRenderer renderer(provider);

  renderer.begin_epoch(120);
  const auto small = renderer.request_frame(make_title_snapshot("HI", 24.0, false, false, 96, 96),
                                            edit::Time{}, {}, 120);
  ASSERT_TRUE(small) << small.error->message;

  renderer.begin_epoch(121);
  const auto large = renderer.request_frame(make_title_snapshot("HI", 96.0, false, false, 96, 96),
                                            edit::Time{}, {}, 121);
  ASSERT_TRUE(large) << large.error->message;

  renderer.begin_epoch(122);
  const auto bold_italic = renderer.request_frame(
      make_title_snapshot("HI", 96.0, true, true, 96, 96), edit::Time{}, {}, 122);
  ASSERT_TRUE(bold_italic) << bold_italic.error->message;

  const auto count_foreground_pixels = [](const std::shared_ptr<const CpuFrame>& frame) {
    int lit_pixels = 0;
    for (int y = 0; y < frame->height(); ++y) {
      for (int x = 0; x < frame->width(); ++x) {
        const auto pixel = frame->pixel(x, y);
        if (pixel[0] > 0.01F || pixel[1] > 0.01F || pixel[2] > 0.01F) {
          ++lit_pixels;
        }
      }
    }
    return lit_pixels;
  };

  const auto rightmost_foreground = [](const std::shared_ptr<const CpuFrame>& frame) {
    int rightmost = -1;
    for (int y = 0; y < frame->height(); ++y) {
      for (int x = 0; x < frame->width(); ++x) {
        const auto pixel = frame->pixel(x, y);
        if (pixel[0] > 0.01F || pixel[1] > 0.01F || pixel[2] > 0.01F) {
          rightmost = std::max(rightmost, x);
        }
      }
    }
    return rightmost;
  };

  const auto small_frame = std::get<std::shared_ptr<const CpuFrame>>(small.value->storage);
  const auto large_frame = std::get<std::shared_ptr<const CpuFrame>>(large.value->storage);
  const auto styled_frame = std::get<std::shared_ptr<const CpuFrame>>(bold_italic.value->storage);
  EXPECT_LT(count_foreground_pixels(small_frame), count_foreground_pixels(large_frame));
  EXPECT_GT(count_foreground_pixels(styled_frame), count_foreground_pixels(large_frame));
  EXPECT_GT(rightmost_foreground(styled_frame), rightmost_foreground(large_frame));
  EXPECT_TRUE(provider->requests.empty());
}

TEST(CpuRenderer, EmptyCanonicalTitleTextRendersBlankWithoutProviderDecode) {
  auto provider = std::make_shared<RecordingProvider>();
  CpuRenderer renderer(provider);
  const auto snapshot = make_title_snapshot("", 96.0, false, false, 24, 24, "Legacy Clip Name");
  renderer.begin_epoch(123);
  const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 123);
  ASSERT_TRUE(result) << result.error->message;
  EXPECT_TRUE(provider->requests.empty());
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(result.value->storage);
  EXPECT_EQ(count_foreground_pixels(frame), 0);
}

TEST(CpuRenderer, TitleFontSizeScalesWithPreviewResolution) {
  auto provider = std::make_shared<RecordingProvider>();
  CpuRenderer renderer(provider);
  const auto snapshot = make_title_snapshot("HI", 96.0, false, false, 96, 96);

  renderer.begin_epoch(124);
  const auto full =
      renderer.request_frame(snapshot, edit::Time{}, {.scale = PreviewScale::Full}, 124);
  ASSERT_TRUE(full) << full.error->message;

  renderer.begin_epoch(125);
  const auto half =
      renderer.request_frame(snapshot, edit::Time{}, {.scale = PreviewScale::Half}, 125);
  ASSERT_TRUE(half) << half.error->message;

  renderer.begin_epoch(126);
  const auto quarter =
      renderer.request_frame(snapshot, edit::Time{}, {.scale = PreviewScale::Quarter}, 126);
  ASSERT_TRUE(quarter) << quarter.error->message;

  const auto full_frame = std::get<std::shared_ptr<const CpuFrame>>(full.value->storage);
  const auto half_frame = std::get<std::shared_ptr<const CpuFrame>>(half.value->storage);
  const auto quarter_frame = std::get<std::shared_ptr<const CpuFrame>>(quarter.value->storage);

  EXPECT_GT(title_bbox_width(full_frame), title_bbox_width(half_frame));
  EXPECT_GT(title_bbox_width(half_frame), title_bbox_width(quarter_frame));
  EXPECT_GT(count_foreground_pixels(quarter_frame), 0);
  EXPECT_TRUE(provider->requests.empty());
}

TEST(CpuRenderer, RejectsStaleEpochWhenDecodeBecomesObsoleteMidRequest) {
  const auto asset_id = edit::EntityId::generate();
  auto source = std::make_shared<CpuFrame>(2, 2);
  source->clear(1.0F, 0.0F, 0.0F, 1.0F);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[asset_id] = source;
  edit::Transform transform;
  const auto snapshot = make_single_snapshot(asset_id, 2, 2, transform);
  CpuRenderer renderer(provider);
  renderer.begin_epoch(13);
  provider->on_request = [&renderer]() { renderer.begin_epoch(14); };
  const auto result = renderer.request_frame(snapshot, edit::Time{}, {}, 13);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error->code, RenderErrorCode::StaleRequest);
}

TEST(CpuRenderer, CrossDissolveUsesHandleExtrapolationAndHalfOpenRange) {
  const auto snapshot = make_transition_snapshot(edit::TransitionKind::CrossDissolve);

  const auto& sequence = snapshot.sequence();
  const auto& outgoing_clip = sequence.tracks.front().clips.front();
  const auto& incoming_clip = sequence.tracks.front().clips.back();
  auto red = std::make_shared<CpuFrame>(2, 2);
  red->clear(1.0F, 0.0F, 0.0F, 1.0F);
  auto blue = std::make_shared<CpuFrame>(2, 2);
  blue->clear(0.0F, 0.0F, 1.0F, 1.0F);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[outgoing_clip.asset_id] = red;
  provider->frames[incoming_clip.asset_id] = blue;
  CpuRenderer renderer(provider);
  renderer.begin_epoch(15);
  const auto cut_result = renderer.request_frame(snapshot, edit::Time(10, 1), {}, 15);
  ASSERT_TRUE(cut_result) << cut_result.error->message;
  ASSERT_EQ(provider->requests.size(), 2U);
  EXPECT_EQ(provider->requests[0].source_time, edit::Time(110, 1));
  EXPECT_EQ(provider->requests[1].source_time, edit::Time(200, 1));
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(cut_result.value->storage);
  EXPECT_NEAR(frame->pixel(0, 0)[0], 0.5F, 0.0001F);
  EXPECT_NEAR(frame->pixel(0, 0)[2], 0.5F, 0.0001F);

  provider->requests.clear();
  renderer.begin_epoch(16);
  const auto post_range = renderer.request_frame(snapshot, edit::Time(12, 1), {}, 16);
  ASSERT_TRUE(post_range) << post_range.error->message;
  ASSERT_EQ(provider->requests.size(), 1U);
  EXPECT_EQ(provider->requests[0].asset_id, incoming_clip.asset_id);
}

TEST(CpuRenderer, DipToBlackIsOpaqueAtSharedCut) {
  const auto snapshot = make_transition_snapshot(edit::TransitionKind::DipToBlack);

  const auto& sequence = snapshot.sequence();
  const auto& outgoing_clip = sequence.tracks.front().clips.front();
  const auto& incoming_clip = sequence.tracks.front().clips.back();
  auto red = std::make_shared<CpuFrame>(2, 2);
  red->clear(1.0F, 0.0F, 0.0F, 1.0F);
  auto blue = std::make_shared<CpuFrame>(2, 2);
  blue->clear(0.0F, 0.0F, 1.0F, 1.0F);
  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[outgoing_clip.asset_id] = red;
  provider->frames[incoming_clip.asset_id] = blue;
  CpuRenderer renderer(provider);
  renderer.begin_epoch(17);
  const auto cut_result = renderer.request_frame(snapshot, edit::Time(10, 1), {}, 17);
  ASSERT_TRUE(cut_result) << cut_result.error->message;
  const auto frame = std::get<std::shared_ptr<const CpuFrame>>(cut_result.value->storage);
  const auto pixel = frame->pixel(0, 0);
  EXPECT_FLOAT_EQ(pixel[0], 0.0F);
  EXPECT_FLOAT_EQ(pixel[1], 0.0F);
  EXPECT_FLOAT_EQ(pixel[2], 0.0F);
  EXPECT_FLOAT_EQ(pixel[3], 1.0F);
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

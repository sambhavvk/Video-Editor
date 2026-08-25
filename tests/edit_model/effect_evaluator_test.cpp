// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/effect_evaluator.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace video_editor::edit {
namespace {

EffectParameter parameter(double base) {
  return EffectParameter{.id = "amount", .value = base};
}

Keyframe keyframe(std::int64_t time, double value, KeyframeInterpolation interpolation) {
  return Keyframe{.time = Time(time, 1), .value = value, .interpolation = interpolation};
}

void expect_rejected_effect(Effect effect) {
  Project project;
  Asset asset;
  asset.duration = Time(10, 1);
  asset.has_video = true;
  project.assets.push_back(asset);
  Sequence sequence;
  Track track;
  track.kind = TrackKind::Video;
  Clip clip;
  clip.asset_id = asset.id;
  clip.timeline_range = {Time(0, 1), Time(10, 1)};
  clip.source_range = clip.timeline_range;
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  project.sequences.push_back(std::move(sequence));
  EXPECT_THROW(static_cast<void>(TimelineEditor(std::move(project))), std::invalid_argument);
}

TEST(EffectEvaluator, UsesBaseBeforeFirstAndLastAfterFinalKeyframe) {
  auto value = parameter(-1.0);
  value.keyframes = {keyframe(2, 3.0, KeyframeInterpolation::Linear),
                     keyframe(4, 7.0, KeyframeInterpolation::Linear)};
  ASSERT_TRUE(validateEffectParameter(value, Time(10, 1)).has_value() == false);
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(value, Time(1, 1))), -1.0);
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(value, Time(2, 1))), 3.0);
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(value, Time(8, 1))), 7.0);
}

TEST(EffectEvaluator, LinearAndHoldUseLeftKeyframeInterpolation) {
  auto linear = parameter(0.0);
  linear.keyframes = {keyframe(0, 0.0, KeyframeInterpolation::Linear),
                      keyframe(2, 10.0, KeyframeInterpolation::Linear)};
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(linear, Time(1, 1))), 5.0);

  auto hold = parameter(0.0);
  hold.keyframes = {keyframe(0, 0.0, KeyframeInterpolation::Hold),
                    keyframe(2, 10.0, KeyframeInterpolation::Linear)};
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(hold, Time(1, 1))), 0.0);
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluateEffectParameter(hold, Time(2, 1))), 10.0);
}

TEST(EffectEvaluator, BezierUsesNormalizedFiniteMonotonicHandles) {
  auto value = parameter(0.0);
  auto left = keyframe(0, 0.0, KeyframeInterpolation::Bezier);
  left.outgoing_control = {0.25, 0.0};
  auto right = keyframe(10, 10.0, KeyframeInterpolation::Linear);
  right.incoming_control = {-0.25, 0.0};
  value.keyframes = {left, right};
  ASSERT_FALSE(validateEffectParameter(value, Time(20, 1)));
  const double halfway = std::get<double>(*evaluateEffectParameter(value, Time(5, 1)));
  EXPECT_NEAR(halfway, 5.0, 1.0e-9);

  right.incoming_control.x = -0.9;
  value.keyframes = {left, right};
  ASSERT_TRUE(validateEffectParameter(value, Time(20, 1)).has_value());
}

TEST(EffectEvaluator, PreservesIntegerTypeWithTiesToEvenRounding) {
  auto value = EffectParameter{.id = "count", .value = std::int64_t{0}};
  value.keyframes = {Keyframe{.time = Time(0, 1),
                              .value = std::int64_t{0},
                              .interpolation = KeyframeInterpolation::Linear},
                     Keyframe{.time = Time(2, 1),
                              .value = std::int64_t{1},
                              .interpolation = KeyframeInterpolation::Linear}};
  const auto evaluated = evaluateEffectParameter(value, Time(1, 1));
  ASSERT_TRUE(evaluated);
  EXPECT_TRUE(std::holds_alternative<std::int64_t>(*evaluated));
  EXPECT_EQ(std::get<std::int64_t>(*evaluated), 0);
}

TEST(EffectEvaluator, RejectsMalformedCurvesBeforeTheyCanBePublished) {
  auto value = parameter(0.0);
  value.keyframes = {keyframe(0, 0.0, KeyframeInterpolation::Linear),
                     keyframe(0, 1.0, KeyframeInterpolation::Linear)};
  EXPECT_TRUE(validateEffectParameter(value, Time(5, 1)).has_value());

  value.keyframes[1].time = Time(5, 1);
  value.keyframes[1].value = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(validateEffectParameter(value, Time(5, 1)).has_value());
}

TEST(EffectEvaluator, RejectsMapMismatchesAndKnownOutOfRangeValuesAtPublish) {
  Effect map_mismatch;
  map_mismatch.type = "video.color";
  map_mismatch.parameters.emplace("wrong", EffectParameter{.id = "exposure", .value = 0.0});
  expect_rejected_effect(std::move(map_mismatch));

  Effect out_of_range;
  out_of_range.type = "video.color";
  out_of_range.parameters.emplace("exposure", EffectParameter{.id = "exposure", .value = 100.0});
  expect_rejected_effect(std::move(out_of_range));
}

TEST(EffectEvaluator, RejectsMalformedCurvesAndEmptyLutPath) {
  Effect malformed_curves;
  malformed_curves.type = "video.curves";
  malformed_curves.parameters.emplace("red", EffectParameter{.id = "red", .value = std::string{"0,0;0,1"}});
  malformed_curves.parameters.emplace("green",
                                      EffectParameter{.id = "green", .value = std::string{"0,0;1,1"}});
  malformed_curves.parameters.emplace("blue",
                                      EffectParameter{.id = "blue", .value = std::string{"0,0;1,1"}});
  malformed_curves.parameters.emplace("luma",
                                      EffectParameter{.id = "luma", .value = std::string{"0,0;1,1"}});
  expect_rejected_effect(std::move(malformed_curves));

  Effect identity_curves;
  identity_curves.type = "video.curves";
  identity_curves.parameters.emplace("red",
                                       EffectParameter{.id = "red", .value = std::string{"0,0;1,1"}});
  identity_curves.parameters.emplace("green",
                                     EffectParameter{.id = "green", .value = std::string{"0,0;1,1"}});
  identity_curves.parameters.emplace("blue",
                                     EffectParameter{.id = "blue", .value = std::string{"0,0;1,1"}});
  identity_curves.parameters.emplace("luma",
                                     EffectParameter{.id = "luma", .value = std::string{"0,0;1,1"}});
  EXPECT_FALSE(validateEffect(identity_curves, Time(10, 1)).has_value());

  Effect empty_lut;
  empty_lut.type = "video.lut";
  empty_lut.parameters.emplace("path", EffectParameter{.id = "path", .value = std::string{}});
  expect_rejected_effect(std::move(empty_lut));

  Effect valid_lut;
  valid_lut.type = "video.lut";
  valid_lut.parameters.emplace("path",
                               EffectParameter{.id = "path", .value = std::string{"/tmp/test.cube"}});
  EXPECT_FALSE(validateEffect(valid_lut, Time(10, 1)).has_value());
}

TEST(EffectEvaluator, RejectsDuplicateKeyframeIdsAcrossEffectState) {
  Effect effect;
  effect.type = "video.color";
  EffectParameter first{.id = "exposure", .value = 0.0};
  EffectParameter second{.id = "contrast", .value = 1.0};
  const EntityId shared_id = EntityId::generate();
  first.keyframes.push_back(Keyframe{.id = shared_id, .time = Time(0, 1), .value = 0.0});
  second.keyframes.push_back(Keyframe{.id = shared_id, .time = Time(1, 1), .value = 1.0});
  effect.parameters.emplace(first.id, std::move(first));
  effect.parameters.emplace(second.id, std::move(second));
  expect_rejected_effect(std::move(effect));
}

} // namespace
} // namespace video_editor::edit

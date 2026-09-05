// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] Project makeProjectWithAsset() {
  Project project;
  Asset asset;
  asset.name = "clip.mov";
  asset.source_uri = "memory://clip";
  asset.duration = Time(100, 1);
  asset.has_video = true;
  asset.width = 1920;
  asset.height = 1080;
  project.assets.push_back(asset);
  Sequence sequence;
  sequence.name = "Main";
  Track video;
  video.kind = TrackKind::Video;
  sequence.tracks.push_back(video);
  project.sequences.push_back(sequence);
  return project;
}

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  const auto result = editor.apply(std::move(command), editor.revision());
  return static_cast<bool>(result);
}

TEST(SchemaV4EditModelTest, CreateMoveAndRemoveFolderBins) {
  auto project = makeProjectWithAsset();
  TimelineEditor editor(std::move(project));

  MediaBin parent;
  parent.name = "Production";
  parent.kind = MediaBinKind::Folder;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateBinCommand{.bin = parent}}));

  MediaBin child;
  child.name = "Interviews";
  child.kind = MediaBinKind::Folder;
  child.parent_id = parent.id;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateBinCommand{.bin = child}}));

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RenameBinCommand{.bin_id = child.id,
                                                                        .name = "B-Roll"}}));

  MediaBin smart;
  smart.name = "Rated";
  smart.kind = MediaBinKind::Smart;
  smart.query.min_rating = 4;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateBinCommand{.bin = smart}}));

  ASSERT_FALSE(applyOk(editor, EditCommand{.operation = MoveBinCommand{.bin_id = smart.id,
                                                                       .parent_id = parent.id}}));

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RemoveBinCommand{.bin_id = smart.id}}));
  ASSERT_FALSE(applyOk(editor, EditCommand{.operation = RemoveBinCommand{.bin_id = parent.id}}));
}

TEST(SchemaV4EditModelTest, SetAssetMetadataAndFolderBinAssignment) {
  auto project = makeProjectWithAsset();
  const auto asset_id = project.assets.front().id;
  TimelineEditor editor(std::move(project));

  MediaBin folder;
  folder.name = "Camera";
  folder.kind = MediaBinKind::Folder;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateBinCommand{.bin = folder}}));

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetAssetMetadataCommand{
                                              .asset_id = asset_id,
                                              .display_title = "Opening shot",
                                              .tags = {"hero", "day"},
                                              .notes = "Slate A",
                                              .rating = 5}}));

  ASSERT_TRUE(applyOk(editor,
                      EditCommand{.operation = SetAssetBinCommand{.asset_id = asset_id,
                                                                  .bin_id = folder.id}}));

  const auto* asset = findAsset(*editor.projectAt(editor.revision()), asset_id);
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(asset->display_title, "Opening shot");
  EXPECT_EQ(asset->tags, (std::vector<std::string>{"hero", "day"}));
  EXPECT_EQ(asset->rating, 5);
  EXPECT_EQ(asset->bin_id, folder.id);
}

TEST(SchemaV4EditModelTest, SmartBinsDoNotOwnAssets) {
  auto project = makeProjectWithAsset();
  const auto asset_id = project.assets.front().id;
  TimelineEditor editor(std::move(project));

  MediaBin smart;
  smart.name = "Favorites";
  smart.kind = MediaBinKind::Smart;
  smart.query.tags = {"hero"};
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateBinCommand{.bin = smart}}));

  ASSERT_FALSE(applyOk(editor,
                       EditCommand{.operation = SetAssetBinCommand{.asset_id = asset_id,
                                                                   .bin_id = smart.id}}));
}

TEST(SchemaV4EditModelTest, RejectsNestedSequenceCyclesAndDepth) {
  Project project;
  Sequence outer;
  outer.name = "Outer";
  Sequence inner;
  inner.name = "Inner";
  Sequence deepest;
  deepest.name = "Deep";

  Track outer_track;
  outer_track.kind = TrackKind::Video;
  Track inner_track;
  inner_track.kind = TrackKind::Video;
  Track deep_track;
  deep_track.kind = TrackKind::Video;

  Clip nested;
  nested.kind = ClipKind::NestedSequence;
  nested.name = "Nest";
  nested.timeline_range = TimeRange(Time{}, Time(10, 1));
  nested.source_range = TimeRange(Time{}, Time(10, 1));
  nested.nested_sequence_id = inner.id;
  outer_track.clips.push_back(nested);

  Clip cycle;
  cycle.kind = ClipKind::NestedSequence;
  cycle.name = "Cycle";
  cycle.timeline_range = TimeRange(Time{}, Time(10, 1));
  cycle.source_range = TimeRange(Time{}, Time(10, 1));
  cycle.nested_sequence_id = outer.id;
  inner_track.clips.push_back(cycle);

  outer.tracks.push_back(outer_track);
  inner.tracks.push_back(inner_track);
  deepest.tracks.push_back(deep_track);
  project.sequences = {outer, inner, deepest};

  EXPECT_THROW(TimelineEditor(std::move(project)), std::invalid_argument);

  Project deep_project;
  std::vector<Sequence> sequences;
  for (int index = 0; index < 10; ++index) {
    Sequence sequence;
    sequence.name = "Level";
    sequence.width = 1920;
    sequence.height = 1080;
    sequence.audio_sample_rate = 48'000;
    sequences.push_back(sequence);
  }
  for (int index = 0; index < 9; ++index) {
    Track track;
    track.kind = TrackKind::Video;
    Clip clip;
    clip.kind = ClipKind::NestedSequence;
    clip.name = "Nest";
    clip.timeline_range = TimeRange(Time{}, Time(10, 1));
    clip.source_range = TimeRange(Time{}, Time(10, 1));
    clip.nested_sequence_id = sequences[static_cast<std::size_t>(index + 1)].id;
    track.clips.push_back(clip);
    sequences[static_cast<std::size_t>(index)].tracks.push_back(track);
  }
  deep_project.sequences = std::move(sequences);
  EXPECT_THROW(TimelineEditor(std::move(deep_project)), std::invalid_argument);
}

} // namespace
} // namespace video_editor::edit

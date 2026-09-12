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
  asset.fingerprint = "abc123";
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
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(F01ProductionMetadataTest, SceneTakeSearchAndRenamePreservesIdentity) {
  auto project = makeProjectWithAsset();
  const auto asset_id = project.assets.front().id;
  const std::string original_uri = project.assets.front().source_uri;
  const std::string original_fingerprint = project.assets.front().fingerprint;
  TimelineEditor editor(std::move(project));

  ProductionMetadata production;
  production.scene = "12";
  production.shot = "3A";
  production.take = "2";
  production.camera = "A";
  production.preferred_take = true;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetAssetMetadataCommand{
                                              .asset_id = asset_id,
                                              .display_title = "Slate rename",
                                              .production = production}}));

  const auto* asset = findAsset(*editor.projectAt(editor.revision()), asset_id);
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(asset->display_title, "Slate rename");
  EXPECT_EQ(asset->production.scene, "12");
  EXPECT_EQ(asset->production.take, "2");
  EXPECT_EQ(asset->source_uri, original_uri);
  EXPECT_EQ(asset->fingerprint, original_fingerprint);

  SmartQuery query;
  query.scene_equals = "12";
  query.take_equals = "2";
  EXPECT_TRUE(assetMatchesSmartQuery(*asset, query));

  SmartQuery other;
  other.scene_equals = "99";
  EXPECT_FALSE(assetMatchesSmartQuery(*asset, other));
}

TEST(F01ProductionMetadataTest, UpsertSavedMediaViewIsUndoable) {
  auto project = makeProjectWithAsset();
  TimelineEditor editor(std::move(project));

  SavedMediaView view;
  view.name = "Dailies";
  view.visible_columns = {"scene", "take"};
  view.search.scene_equals = "12";
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = UpsertSavedMediaViewCommand{.view = view}}));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetActiveMediaViewCommand{.view_id = view.id}}));

  const auto current = editor.projectAt(editor.revision());
  EXPECT_EQ(current->saved_media_views.size(), 1U);
  EXPECT_EQ(current->active_media_view_id, view.id);

  const auto revision_before_undo = editor.revision();
  ASSERT_TRUE(editor.undo(revision_before_undo));
  const auto after_undo = editor.projectAt(editor.revision());
  EXPECT_FALSE(after_undo->active_media_view_id.has_value());
}

} // namespace
} // namespace video_editor::edit

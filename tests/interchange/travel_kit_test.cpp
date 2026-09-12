// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/travel_kit.h"
#include "video_editor/project_codec/project_codec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace video_editor::interchange {
namespace {

TEST(TravelKitTest, WritesProjectAndManifest) {
  edit::Project project;
  project.name = "Travel";
  const auto bytes = project_codec::serialize_project(project);
  const auto temp = std::filesystem::temp_directory_path() / "ve_travel_kit_test";
  std::filesystem::remove_all(temp);
  const auto result = build_travel_kit(project, bytes, temp);
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::filesystem::exists(result.value().project_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().manifest_path));
  std::filesystem::remove_all(temp);
}

TEST(TravelKitTest, BundlesRelativeMediaAndOmitsRemoteUris) {
  const auto source_dir = std::filesystem::temp_directory_path() / "ve_travel_kit_src";
  std::filesystem::remove_all(source_dir);
  std::filesystem::create_directories(source_dir);
  const auto media = source_dir / "clip.bin";
  {
    std::ofstream out(media, std::ios::binary);
    out << "media";
  }

  edit::Project project;
  project.name = "Travel";
  edit::Asset local;
  local.name = "clip.bin";
  local.source_uri = media.generic_string();
  local.duration = edit::Time(24, 1);
  local.has_video = true;
  local.width = 1920;
  local.height = 1080;
  project.assets.push_back(local);
  edit::Asset remote;
  remote.name = "stock";
  remote.source_uri = "https://example.invalid/stock.mov";
  remote.duration = edit::Time(24, 1);
  project.assets.push_back(remote);

  const auto bytes = project_codec::serialize_project(project);
  const auto temp = std::filesystem::temp_directory_path() / "ve_travel_kit_rel";
  std::filesystem::remove_all(temp);
  const auto result = build_travel_kit(project, bytes, temp);
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result.value().bundled_assets.size(), 1U);
  EXPECT_EQ(result.value().bundled_assets.front(), "media/clip.bin");
  ASSERT_EQ(result.value().omitted_dependencies.size(), 1U);
  EXPECT_EQ(result.value().omitted_dependencies.front(), remote.source_uri);
  EXPECT_TRUE(std::filesystem::exists(temp / "media" / "clip.bin"));

  std::ifstream manifest(result.value().manifest_path);
  const std::string manifest_text((std::istreambuf_iterator<char>(manifest)),
                                  std::istreambuf_iterator<char>());
  EXPECT_NE(manifest_text.find("Bundled:"), std::string::npos);
  EXPECT_NE(manifest_text.find("media/clip.bin"), std::string::npos);
  EXPECT_NE(manifest_text.find("Omitted:"), std::string::npos);
  EXPECT_NE(manifest_text.find(remote.source_uri), std::string::npos);

  const auto rewritten = project_codec::deserialize_project(
      [&] {
        std::ifstream in(result.value().project_path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        project_codec::ProjectBytes out;
        out.reserve(raw.size());
        for (const char byte : raw) {
          out.push_back(static_cast<std::byte>(byte));
        }
        return out;
      }());
  ASSERT_TRUE(rewritten);
  ASSERT_EQ(rewritten.value().assets.size(), 2U);
  EXPECT_EQ(rewritten.value().assets.front().source_uri, "media/clip.bin");
  EXPECT_EQ(rewritten.value().assets.back().source_uri, remote.source_uri);

  std::filesystem::remove_all(temp);
  std::filesystem::remove_all(source_dir);
}

} // namespace
} // namespace video_editor::interchange

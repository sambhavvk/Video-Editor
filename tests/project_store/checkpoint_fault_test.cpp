// SPDX-License-Identifier: MPL-2.0

#include "video_editor/project_store/project_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace video_editor::store {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video-editor-checkpoint-fault-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::filesystem::path file(const std::string_view name) const {
    return path_ / name;
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class DirectoryPermissionGuard final {
public:
  explicit DirectoryPermissionGuard(std::filesystem::path path)
      : path_(std::move(path)), previous_(std::filesystem::status(path_).permissions()) {}

  ~DirectoryPermissionGuard() {
    std::error_code ignored;
    std::filesystem::permissions(path_, previous_, std::filesystem::perm_options::replace, ignored);
  }

  DirectoryPermissionGuard(const DirectoryPermissionGuard&) = delete;
  DirectoryPermissionGuard& operator=(const DirectoryPermissionGuard&) = delete;

  [[nodiscard]] bool make_read_only_for_owner() {
    std::error_code error;
    std::filesystem::permissions(path_,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace, error);
    return !error;
  }

private:
  std::filesystem::path path_;
  std::filesystem::perms previous_;
};

[[nodiscard]] bool directory_rejects_create(const std::filesystem::path& directory) {
  const auto probe = directory / ".write-probe";
  {
    std::ofstream output(probe);
    if (static_cast<bool>(output)) {
      output.close();
      std::error_code ignored;
      std::filesystem::remove(probe, ignored);
      return false;
    }
  }
  return true;
}

TEST(ProjectStoreCheckpointFault, MissingParentLeavesWorkingDatabaseValid) {
  TemporaryDirectory temporary;
  ProjectStore store(temporary.file("working.sqlite"));
  EXPECT_EQ(store.append_command("edit", "payload", 0), 1U);

  EXPECT_THROW(static_cast<void>(store.checkpoint_to(
                   temporary.file("missing-parent") / "project.veproj", 1)),
               ProjectStoreError);

  EXPECT_TRUE(store.quick_check().ok());
  EXPECT_EQ(store.metadata().head_revision, 1U);
  EXPECT_EQ(store.metadata().saved_revision, 0U);
  EXPECT_EQ(store.append_command("after-fault", "still-writable", 1), 2U);
}

TEST(ProjectStoreCheckpointFault, ParentFileLeavesWorkingDatabaseValid) {
  TemporaryDirectory temporary;
  const auto parent_file = temporary.file("not-a-directory");
  {
    std::ofstream output(parent_file);
    ASSERT_TRUE(static_cast<bool>(output));
    output << "regular file";
  }
  ProjectStore store(temporary.file("working.sqlite"));
  EXPECT_EQ(store.append_command("edit", "payload", 0), 1U);

  EXPECT_THROW(static_cast<void>(store.checkpoint_to(parent_file / "project.veproj", 1)),
               ProjectStoreError);

  EXPECT_TRUE(store.quick_check().ok());
  EXPECT_EQ(store.metadata().head_revision, 1U);
  EXPECT_EQ(store.metadata().saved_revision, 0U);
  EXPECT_TRUE(std::filesystem::is_regular_file(parent_file));
}

TEST(ProjectStoreCheckpointFault, FailedOverwriteDoesNotReplaceGoodVeproj) {
#ifdef _WIN32
  GTEST_SKIP() << "read-only directory injection is Linux-first";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "read-only directory injection is skipped when running as root";
  }

  TemporaryDirectory temporary;
  const auto working_path = temporary.file("working.sqlite");
  const auto checkpoint_dir = temporary.file("checkpoints");
  std::filesystem::create_directories(checkpoint_dir);
  const auto checkpoint_path = checkpoint_dir / "project.veproj";

  ProjectStore store(working_path);
  EXPECT_EQ(store.append_command("first", "one", 0), 1U);
  EXPECT_EQ(store.checkpoint_to(checkpoint_path, 1), 1U);
  ASSERT_TRUE(std::filesystem::is_regular_file(checkpoint_path));
  const std::string original = read_bytes(checkpoint_path);
  ASSERT_FALSE(original.empty());

  EXPECT_EQ(store.append_command("second", "two", 1), 2U);

  DirectoryPermissionGuard guard(checkpoint_dir);
  ASSERT_TRUE(guard.make_read_only_for_owner());
  if (!directory_rejects_create(checkpoint_dir)) {
    GTEST_SKIP() << "environment does not honor directory write permissions";
  }

  try {
    static_cast<void>(store.checkpoint_to(checkpoint_path, 2));
    FAIL() << "Expected checkpoint_to to fail against a read-only destination";
  } catch (const ProjectStoreError&) {
  } catch (const SqliteError&) {
  }

  EXPECT_TRUE(store.quick_check().ok());
  EXPECT_EQ(store.metadata().head_revision, 2U);
  EXPECT_EQ(store.metadata().saved_revision, 1U);
  EXPECT_EQ(store.append_command("after-fault", "still-writable", 2), 3U);

  std::error_code restore_error;
  std::filesystem::permissions(checkpoint_dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add, restore_error);
  ASSERT_FALSE(restore_error);
  EXPECT_TRUE(std::filesystem::is_regular_file(checkpoint_path));
  EXPECT_EQ(read_bytes(checkpoint_path), original);

  ProjectStore previous(checkpoint_path);
  EXPECT_EQ(previous.metadata().head_revision, 1U);
  EXPECT_EQ(previous.metadata().saved_revision, 1U);
  ASSERT_EQ(previous.read_commands().size(), 1U);
  EXPECT_EQ(previous.read_commands().front().command_type, "first");
  EXPECT_TRUE(previous.quick_check().ok());
#endif
}

} // namespace
} // namespace video_editor::store

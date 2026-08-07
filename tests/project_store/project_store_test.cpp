// SPDX-License-Identifier: MPL-2.0

#include "video_editor/project_store/project_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace video_editor::store {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video-editor-project-store-test-" + std::to_string(timestamp) + "-" +
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

std::string query_text(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(db));
  }
  const int result = sqlite3_step(statement);
  if (result != SQLITE_ROW) {
    const std::string message = sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    throw std::runtime_error(message);
  }
  const auto* value = sqlite3_column_text(statement, 0);
  const std::string text = value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
  sqlite3_finalize(statement);
  return text;
}

sqlite3_int64 query_int64(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(db));
  }
  const int result = sqlite3_step(statement);
  if (result != SQLITE_ROW) {
    const std::string message = sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    throw std::runtime_error(message);
  }
  const sqlite3_int64 value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

void set_heartbeat(const std::filesystem::path& path, const std::int64_t heartbeat_utc_ms) {
  SqliteConnection connection(path, SqliteOpenMode::kReadWrite);
  connection.execute("UPDATE project_metadata SET heartbeat_utc_ms = " +
                     std::to_string(heartbeat_utc_ms) + " WHERE singleton = 1");
}

TEST(ProjectStoreTest, CreatesVersionOneWalDatabaseAndMetadata) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("working.sqlite");
  constexpr std::string_view kProjectUuid = "019ab123-4567-7abc-8def-0123456789ab";

  ProjectStore store(path, OpenOptions{.project_uuid = std::string(kProjectUuid)});
  const ProjectMetadata metadata = store.metadata();

  EXPECT_EQ(metadata.project_uuid, kProjectUuid);
  EXPECT_EQ(metadata.schema_version, kCurrentProjectSchemaVersion);
  EXPECT_EQ(metadata.head_revision, 0U);
  EXPECT_EQ(metadata.saved_revision, 0U);
  EXPECT_FALSE(metadata.clean_close);
  EXPECT_GT(metadata.heartbeat_utc_ms, 0);
  EXPECT_TRUE(store.recovery_status().integrity_ok);
  EXPECT_TRUE(store.recovery_status().previous_clean_close);
  EXPECT_FALSE(store.recovery_status().recovery_recommended);
  EXPECT_TRUE(store.quick_check().ok());

  SqliteConnection inspection(path, SqliteOpenMode::kReadOnly);
  EXPECT_EQ(query_text(inspection.native_handle(), "PRAGMA journal_mode"), "wal");
  EXPECT_EQ(query_text(inspection.native_handle(), "PRAGMA user_version"), "1");
  EXPECT_EQ(query_text(inspection.native_handle(), "SELECT count(*) FROM schema_migrations"), "1");
}

TEST(ProjectStoreTest, GeneratesAUuidV7WhenNoneIsProvided) {
  TemporaryDirectory temporary;
  ProjectStore store(temporary.file("working.sqlite"));

  const std::string uuid = store.metadata().project_uuid;
  ASSERT_EQ(uuid.size(), 36U);
  EXPECT_EQ(uuid[8], '-');
  EXPECT_EQ(uuid[13], '-');
  EXPECT_EQ(uuid[14], '7');
  EXPECT_EQ(uuid[18], '-');
  EXPECT_NE(std::string("89ab").find(uuid[19]), std::string::npos);
  EXPECT_EQ(uuid[23], '-');
}

TEST(ProjectStoreTest, PersistsTextAndBinaryJournalEntriesAcrossReopen) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("working.sqlite");
  const std::string text_payload("clip\0payload", 12);
  constexpr std::array<std::byte, 4> kBinaryPayload{std::byte{0x00}, std::byte{0x7f},
                                                    std::byte{0x80}, std::byte{0xff}};

  {
    ProjectStore store(path);
    EXPECT_EQ(store.append_command("insert_clip", text_payload, 0), 1U);
    EXPECT_EQ(store.append_command("set_effect", kBinaryPayload, 1), 2U);
    EXPECT_EQ(store.append_command("empty_blob", std::span<const std::byte>{}, 2), 3U);
    store.mark_clean_close(3);
  }

  ProjectStore reopened(path);
  const auto entries = reopened.read_commands();
  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].revision, 1U);
  EXPECT_EQ(entries[0].command_type, "insert_clip");
  ASSERT_TRUE(std::holds_alternative<std::string>(entries[0].payload));
  EXPECT_EQ(std::get<std::string>(entries[0].payload), text_payload);
  EXPECT_EQ(entries[1].revision, 2U);
  EXPECT_EQ(entries[1].command_type, "set_effect");
  ASSERT_TRUE(std::holds_alternative<BinaryPayload>(entries[1].payload));
  const BinaryPayload expected(kBinaryPayload.begin(), kBinaryPayload.end());
  EXPECT_EQ(std::get<BinaryPayload>(entries[1].payload), expected);
  EXPECT_EQ(entries[2].revision, 3U);
  ASSERT_TRUE(std::holds_alternative<BinaryPayload>(entries[2].payload));
  EXPECT_TRUE(std::get<BinaryPayload>(entries[2].payload).empty());
  EXPECT_TRUE(reopened.recovery_status().previous_clean_close);
  EXPECT_TRUE(reopened.recovery_status().had_unsaved_changes);
  EXPECT_TRUE(reopened.recovery_status().recovery_recommended);
}

TEST(ProjectStoreTest, InvalidInitialUuidDoesNotLeaveAPartialSchema) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("working.sqlite");

  EXPECT_THROW(
      { ProjectStore invalid(path, OpenOptions{.project_uuid = std::string{}}); },
      std::invalid_argument);

  ProjectStore valid(path);
  EXPECT_EQ(valid.metadata().schema_version, kCurrentProjectSchemaVersion);
  EXPECT_TRUE(valid.quick_check().ok());
}

TEST(ProjectStoreTest, RejectsAStaleExpectedRevisionWithoutPartialWrite) {
  TemporaryDirectory temporary;
  ProjectStore store(temporary.file("working.sqlite"));
  EXPECT_EQ(store.append_command("first", "{}", 0), 1U);

  try {
    static_cast<void>(store.append_command("stale", "{}", 0));
    FAIL() << "Expected a revision conflict";
  } catch (const RevisionConflict& conflict) {
    EXPECT_EQ(conflict.expected(), 0U);
    EXPECT_EQ(conflict.actual(), 1U);
  }

  EXPECT_EQ(store.metadata().head_revision, 1U);
  const auto entries = store.read_commands();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries.front().command_type, "first");
}

TEST(ProjectStoreTest, ReportsUncleanUnsavedWorkingDatabaseOnReopen) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("working.sqlite");
  {
    ProjectStore store(path);
    EXPECT_EQ(store.append_command("insert", "payload", 0), 1U);
    // No mark_clean_close call simulates process termination after commit.
  }

  ProjectStore reopened(path);
  const RecoveryStatus status = reopened.recovery_status();
  EXPECT_FALSE(status.previous_clean_close);
  EXPECT_TRUE(status.had_unsaved_changes);
  EXPECT_TRUE(status.recovery_recommended);
  EXPECT_EQ(status.head_revision, 1U);
  EXPECT_EQ(status.saved_revision, 0U);
}

TEST(ProjectStoreTest, CreatesAndAtomicallyReplacesSelfContainedCheckpoints) {
  TemporaryDirectory temporary;
  const auto working_path = temporary.file("working.sqlite");
  const auto checkpoint_path = temporary.file("project.veproj");

  {
    ProjectStore store(working_path);
    EXPECT_EQ(store.append_command("first", "one", 0), 1U);
    EXPECT_EQ(store.checkpoint_to(checkpoint_path, 1), 1U);
    EXPECT_EQ(store.metadata().saved_revision, 1U);
    EXPECT_EQ(store.append_command("second", "two", 1), 2U);
    EXPECT_EQ(store.checkpoint_to(checkpoint_path, 2), 2U);
    EXPECT_EQ(store.metadata().saved_revision, 2U);
    store.mark_clean_close(2);
  }

  ProjectStore checkpoint(checkpoint_path);
  const ProjectMetadata metadata = checkpoint.metadata();
  EXPECT_EQ(metadata.head_revision, 2U);
  EXPECT_EQ(metadata.saved_revision, 2U);
  EXPECT_TRUE(checkpoint.recovery_status().previous_clean_close);
  EXPECT_FALSE(checkpoint.recovery_status().had_unsaved_changes);
  EXPECT_FALSE(checkpoint.recovery_status().recovery_recommended);
  ASSERT_EQ(checkpoint.read_commands().size(), 2U);
  EXPECT_TRUE(checkpoint.quick_check().ok());
}

TEST(ProjectStoreTest, RejectsCheckpointToTheOpenWorkingDatabase) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("working.sqlite");
  ProjectStore store(path);

  EXPECT_THROW(static_cast<void>(store.checkpoint_to(path, 0)), ProjectStoreError);
  EXPECT_TRUE(store.quick_check().ok());
}

TEST(ProjectStoreTest, MigratesAnEmptyVersionZeroDatabaseForward) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("legacy.sqlite");
  {
    SqliteConnection legacy(path);
    legacy.execute("PRAGMA user_version = 0");
  }

  ProjectStore migrated(path);
  EXPECT_EQ(migrated.metadata().schema_version, kCurrentProjectSchemaVersion);
  EXPECT_TRUE(migrated.quick_check().ok());
  SqliteConnection inspection(path, SqliteOpenMode::kReadOnly);
  EXPECT_EQ(query_text(inspection.native_handle(), "PRAGMA user_version"), "1");
}

TEST(ProjectStoreTest, RejectsUnknownAndFutureSchemas) {
  TemporaryDirectory temporary;
  const auto unknown_path = temporary.file("unknown.sqlite");
  {
    SqliteConnection unknown(unknown_path);
    unknown.execute("CREATE TABLE mystery(value INTEGER)");
  }
  EXPECT_THROW({ ProjectStore store(unknown_path); }, ProjectStoreError);

  const auto future_path = temporary.file("future.sqlite");
  {
    SqliteConnection future(future_path);
    future.execute("PRAGMA user_version = 999");
  }
  EXPECT_THROW({ ProjectStore store(future_path); }, ProjectStoreError);
}

TEST(ProjectStoreTest, ConditionalMetadataUpdatesDoNotAdvanceRevision) {
  TemporaryDirectory temporary;
  ProjectStore store(temporary.file("working.sqlite"));
  EXPECT_EQ(store.append_command("edit", "payload", 0), 1U);

  store.update_heartbeat();
  store.mark_saved(1);
  store.mark_clean_close(1);

  const ProjectMetadata metadata = store.metadata();
  EXPECT_EQ(metadata.head_revision, 1U);
  EXPECT_EQ(metadata.saved_revision, 1U);
  EXPECT_TRUE(metadata.clean_close);
  EXPECT_THROW(store.mark_saved(0), RevisionConflict);
  EXPECT_EQ(store.read_commands().size(), 1U);
}

TEST(RecoveryCatalogTest, ReportsCleanDatabaseWithoutMutatingItsMetadata) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("clean.working.sqlite");
  constexpr std::string_view kProjectUuid = "019ab123-4567-7abc-8def-0123456789ab";
  ProjectMetadata before;
  {
    ProjectStore store(path, OpenOptions{.project_uuid = std::string(kProjectUuid)});
    store.mark_clean_close(0);
    before = store.metadata();
  }

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  ASSERT_FALSE(catalog.truncated);
  ASSERT_EQ(catalog.candidates.size(), 1U);
  const RecoveryCandidate& candidate = catalog.candidates.front();
  EXPECT_EQ(candidate.working_database, path);
  EXPECT_EQ(candidate.project_uuid, kProjectUuid);
  EXPECT_EQ(candidate.schema_version, kCurrentProjectSchemaVersion);
  EXPECT_EQ(candidate.head_revision, 0U);
  EXPECT_EQ(candidate.saved_revision, 0U);
  EXPECT_TRUE(candidate.clean_close);
  EXPECT_EQ(candidate.heartbeat_utc_ms, before.heartbeat_utc_ms);
  EXPECT_TRUE(candidate.integrity_ok);
  EXPECT_TRUE(candidate.valid_project_database);
  EXPECT_FALSE(candidate.recovery_recommended);
  EXPECT_TRUE(candidate.diagnostic.empty());

  SqliteConnection inspection(path, SqliteOpenMode::kReadOnly);
  EXPECT_EQ(query_int64(inspection.native_handle(),
                        "SELECT clean_close FROM project_metadata WHERE singleton = 1"),
            1);
  EXPECT_EQ(query_int64(inspection.native_handle(),
                        "SELECT heartbeat_utc_ms FROM project_metadata WHERE singleton = 1"),
            before.heartbeat_utc_ms);
}

TEST(RecoveryCatalogTest, RecommendsAnUncleanDatabaseWithUnsavedCommands) {
  TemporaryDirectory temporary;
  const auto path = temporary.file("crashed.working.sqlite");
  {
    ProjectStore store(path);
    EXPECT_EQ(store.append_command("insert_clip", "payload", 0), 1U);
  }

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  ASSERT_EQ(catalog.candidates.size(), 1U);
  const RecoveryCandidate& candidate = catalog.candidates.front();
  EXPECT_TRUE(candidate.valid_project_database);
  EXPECT_TRUE(candidate.integrity_ok);
  EXPECT_FALSE(candidate.clean_close);
  EXPECT_EQ(candidate.head_revision, 1U);
  EXPECT_EQ(candidate.saved_revision, 0U);
  EXPECT_TRUE(candidate.recovery_recommended);
  EXPECT_GT(candidate.heartbeat_utc_ms, 0);
  EXPECT_TRUE(candidate.diagnostic.empty());
}

TEST(RecoveryCatalogTest, IndependentlyRecommendsUnsavedOrUncleanState) {
  TemporaryDirectory temporary;
  const auto clean_unsaved_path = temporary.file("clean-unsaved.working.sqlite");
  const auto unclean_saved_path = temporary.file("unclean-saved.working.sqlite");
  {
    ProjectStore clean_unsaved(clean_unsaved_path);
    EXPECT_EQ(clean_unsaved.append_command("edit", "unsaved", 0), 1U);
    clean_unsaved.mark_clean_close(1);
  }
  {
    ProjectStore unclean_saved(unclean_saved_path);
    EXPECT_EQ(unclean_saved.append_command("edit", "saved", 0), 1U);
    unclean_saved.mark_saved(1);
  }

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  ASSERT_EQ(catalog.candidates.size(), 2U);
  const auto clean_unsaved = std::find_if(catalog.candidates.begin(), catalog.candidates.end(),
                                          [&](const RecoveryCandidate& candidate) {
                                            return candidate.working_database == clean_unsaved_path;
                                          });
  ASSERT_NE(clean_unsaved, catalog.candidates.end());
  EXPECT_TRUE(clean_unsaved->clean_close);
  EXPECT_EQ(clean_unsaved->head_revision, 1U);
  EXPECT_EQ(clean_unsaved->saved_revision, 0U);
  EXPECT_TRUE(clean_unsaved->recovery_recommended);

  const auto unclean_saved = std::find_if(catalog.candidates.begin(), catalog.candidates.end(),
                                          [&](const RecoveryCandidate& candidate) {
                                            return candidate.working_database == unclean_saved_path;
                                          });
  ASSERT_NE(unclean_saved, catalog.candidates.end());
  EXPECT_FALSE(unclean_saved->clean_close);
  EXPECT_EQ(unclean_saved->head_revision, 1U);
  EXPECT_EQ(unclean_saved->saved_revision, 1U);
  EXPECT_TRUE(unclean_saved->recovery_recommended);
}

TEST(RecoveryCatalogTest, KeepsCorruptAndWrongSchemaEntriesWithDiagnostics) {
  TemporaryDirectory temporary;
  const auto corrupt_path = temporary.file("corrupt.working.sqlite");
  {
    std::ofstream corrupt(corrupt_path, std::ios::binary);
    corrupt << "this is not a SQLite database";
  }

  const auto wrong_schema_path = temporary.file("wrong-schema.working.sqlite");
  {
    SqliteConnection wrong_schema(wrong_schema_path);
    wrong_schema.execute("PRAGMA user_version = 1");
    wrong_schema.execute("CREATE TABLE unrelated(value INTEGER)");
  }

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  ASSERT_EQ(catalog.candidates.size(), 2U);
  for (const auto& candidate : catalog.candidates) {
    EXPECT_FALSE(candidate.valid_project_database);
    EXPECT_FALSE(candidate.recovery_recommended);
    EXPECT_FALSE(candidate.diagnostic.empty());
    EXPECT_TRUE(candidate.project_uuid.empty());
  }
  const auto corrupt = std::find_if(catalog.candidates.begin(), catalog.candidates.end(),
                                    [&](const RecoveryCandidate& candidate) {
                                      return candidate.working_database == corrupt_path;
                                    });
  ASSERT_NE(corrupt, catalog.candidates.end());
  EXPECT_FALSE(corrupt->integrity_ok);

  const auto wrong_schema = std::find_if(catalog.candidates.begin(), catalog.candidates.end(),
                                         [&](const RecoveryCandidate& candidate) {
                                           return candidate.working_database == wrong_schema_path;
                                         });
  ASSERT_NE(wrong_schema, catalog.candidates.end());
  EXPECT_TRUE(wrong_schema->integrity_ok);
}

TEST(RecoveryCatalogTest, IgnoresUnrelatedFilesAndDoesNotRecurse) {
  TemporaryDirectory temporary;
  {
    ProjectStore ignored(temporary.file("ordinary.sqlite"));
    ignored.mark_clean_close(0);
  }
  {
    std::ofstream text(temporary.file("notes.txt"));
    text << "not a project";
  }
  const auto nested = temporary.file("nested");
  std::filesystem::create_directory(nested);
  {
    ProjectStore nested_store(nested / "nested.working.sqlite");
    nested_store.mark_clean_close(0);
  }

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  EXPECT_TRUE(catalog.candidates.empty());
  EXPECT_FALSE(catalog.truncated);
}

TEST(RecoveryCatalogTest, SortsRecommendedFirstThenByNewestHeartbeat) {
  TemporaryDirectory temporary;
  const auto older_path = temporary.file("older.working.sqlite");
  const auto newer_path = temporary.file("newer.working.sqlite");
  const auto clean_path = temporary.file("clean.working.sqlite");
  {
    ProjectStore older(older_path);
    EXPECT_EQ(older.append_command("edit", "older", 0), 1U);
  }
  {
    ProjectStore newer(newer_path);
    EXPECT_EQ(newer.append_command("edit", "newer", 0), 1U);
  }
  {
    ProjectStore clean(clean_path);
    clean.mark_clean_close(0);
  }
  set_heartbeat(older_path, 100);
  set_heartbeat(newer_path, 200);
  set_heartbeat(clean_path, 999);

  const RecoveryCatalog catalog = scan_recovery_directory(temporary.path());

  ASSERT_EQ(catalog.candidates.size(), 3U);
  EXPECT_EQ(catalog.candidates[0].working_database, newer_path);
  EXPECT_EQ(catalog.candidates[1].working_database, older_path);
  EXPECT_EQ(catalog.candidates[2].working_database, clean_path);
  EXPECT_TRUE(catalog.candidates[0].recovery_recommended);
  EXPECT_TRUE(catalog.candidates[1].recovery_recommended);
  EXPECT_FALSE(catalog.candidates[2].recovery_recommended);
}

TEST(RecoveryCatalogTest, MissingDirectoryProducesAnEmptyCatalog) {
  TemporaryDirectory temporary;
  const RecoveryCatalog catalog =
      scan_recovery_directory(temporary.file("directory-that-does-not-exist"));

  EXPECT_TRUE(catalog.candidates.empty());
  EXPECT_FALSE(catalog.truncated);
}

TEST(RecoveryCatalogTest, CandidateLimitIsDeterministicAndReported) {
  TemporaryDirectory temporary;
  for (const std::string_view name : {"c.working.sqlite", "a.working.sqlite", "b.working.sqlite"}) {
    const auto path = temporary.file(name);
    ProjectStore store(path);
    store.mark_clean_close(0);
    set_heartbeat(path, 100);
  }

  const RecoveryCatalog catalog =
      scan_recovery_directory(temporary.path(), RecoveryScanOptions{.maximum_candidates = 2});

  ASSERT_TRUE(catalog.truncated);
  ASSERT_EQ(catalog.candidates.size(), 2U);
  EXPECT_EQ(catalog.candidates[0].working_database.filename(), "a.working.sqlite");
  EXPECT_EQ(catalog.candidates[1].working_database.filename(), "b.working.sqlite");
}

} // namespace
} // namespace video_editor::store

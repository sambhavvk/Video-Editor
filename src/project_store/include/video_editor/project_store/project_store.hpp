// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "video_editor/project_store/sqlite_connection.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace video_editor::store {

using Revision = std::uint64_t;
using BinaryPayload = std::vector<std::byte>;
using CommandPayload = std::variant<std::string, BinaryPayload>;

inline constexpr std::uint32_t kCurrentProjectSchemaVersion = 2;
inline constexpr std::uint32_t kMinimumSupportedProjectSchemaVersion = 1;

class ProjectStoreError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class RevisionConflict final : public ProjectStoreError {
public:
  RevisionConflict(Revision expected, Revision actual);

  [[nodiscard]] Revision expected() const noexcept {
    return expected_;
  }
  [[nodiscard]] Revision actual() const noexcept {
    return actual_;
  }

private:
  Revision expected_;
  Revision actual_;
};

struct OpenOptions {
  bool create_if_missing = true;
  bool run_integrity_check = true;
  int busy_timeout_ms = 5'000;
  std::optional<std::string> project_uuid;
};

struct ProjectMetadata {
  std::string project_uuid;
  std::uint32_t schema_version = 0;
  Revision head_revision = 0;
  Revision saved_revision = 0;
  bool clean_close = false;
  std::int64_t heartbeat_utc_ms = 0;
};

struct JournalEntry {
  Revision revision = 0;
  std::string command_type;
  CommandPayload payload;
  std::uint32_t payload_schema_version = 1;
  std::int64_t created_at_utc_ms = 0;
};

struct IntegrityResult {
  std::vector<std::string> messages;

  [[nodiscard]] bool ok() const noexcept;
};

// Describes state found on disk before this process marks the working database
// as open. Unsaved changes and an unclean previous close are independently
// reported so the UI can explain why recovery is being offered.
struct RecoveryStatus {
  bool integrity_checked = false;
  bool integrity_ok = false;
  bool previous_clean_close = false;
  bool had_unsaved_changes = false;
  bool recovery_recommended = false;
  Revision head_revision = 0;
  Revision saved_revision = 0;
  std::int64_t last_heartbeat_utc_ms = 0;
  std::vector<std::string> integrity_messages;
};

// A read-only summary of one abandoned working database discovered by the
// recovery catalog. Invalid databases are retained in the result so the UI can
// explain why an apparent recovery file cannot be opened. Metadata fields are
// left at their defaults when valid_project_database is false.
struct RecoveryCandidate {
  std::filesystem::path working_database;
  std::string project_uuid;
  std::uint32_t schema_version = 0;
  Revision head_revision = 0;
  Revision saved_revision = 0;
  bool clean_close = false;
  std::int64_t heartbeat_utc_ms = 0;
  bool integrity_ok = false;
  bool valid_project_database = false;
  bool recovery_recommended = false;
  std::string diagnostic;
};

struct RecoveryScanOptions {
  // The scanner keeps at most this many matching paths in memory. When a
  // directory contains more matches, the lexicographically earliest paths are
  // inspected so selection remains deterministic across filesystems.
  std::size_t maximum_candidates = 256;
  int busy_timeout_ms = 250;
};

struct RecoveryCatalog {
  std::vector<RecoveryCandidate> candidates;
  bool truncated = false;
};

// Scans only direct children whose names end in ".working.sqlite". The
// databases are opened read-only and are never migrated, marked open, or
// deleted. A missing directory produces an empty catalog.
[[nodiscard]] RecoveryCatalog scan_recovery_directory(const std::filesystem::path& directory,
                                                      RecoveryScanOptions options = {});

class ProjectStore final {
public:
  explicit ProjectStore(std::filesystem::path working_database, OpenOptions options = {});
  // Deliberately does not mark a clean close: the application must call
  // mark_clean_close only after all shutdown work has completed successfully.
  // This makes an omitted call indistinguishable from a crash on the next open.
  ~ProjectStore() = default;

  ProjectStore(const ProjectStore&) = delete;
  ProjectStore& operator=(const ProjectStore&) = delete;
  ProjectStore(ProjectStore&&) noexcept = default;
  ProjectStore& operator=(ProjectStore&&) noexcept = default;

  [[nodiscard]] const std::filesystem::path& working_path() const noexcept {
    return connection_.path();
  }
  [[nodiscard]] ProjectMetadata metadata() const;
  [[nodiscard]] const RecoveryStatus& recovery_status() const noexcept {
    return recovery_status_;
  }
  [[nodiscard]] IntegrityResult quick_check() const;

  Revision append_command(std::string_view command_type, std::string_view payload,
                          Revision expected_revision, std::uint32_t payload_schema_version = 1);
  Revision append_command(std::string_view command_type, std::span<const std::byte> payload,
                          Revision expected_revision, std::uint32_t payload_schema_version = 1);

  [[nodiscard]] std::vector<JournalEntry> read_commands(Revision after_revision = 0) const;

  // Metadata mutations are conditional on the caller's view of the head. They
  // never add journal entries or advance the edit revision.
  void mark_saved(Revision expected_revision);
  void update_heartbeat();
  void mark_clean_close(Revision expected_revision);

  // Writes a self-contained, clean SQLite checkpoint through sqlite3_backup,
  // fsyncs it, atomically replaces destination, then records saved_revision in
  // the working database. Destination must not be the open working database.
  Revision checkpoint_to(const std::filesystem::path& destination, Revision expected_revision);

private:
  Revision append_payload(std::string_view command_type, const CommandPayload& payload,
                          Revision expected_revision, std::uint32_t payload_schema_version);
  void initialize(OpenOptions options);

  SqliteConnection connection_;
  RecoveryStatus recovery_status_;
};

} // namespace video_editor::store

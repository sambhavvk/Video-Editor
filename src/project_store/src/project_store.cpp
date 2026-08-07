// SPDX-License-Identifier: MPL-2.0

#include "video_editor/project_store/project_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace video_editor::store {
namespace {

constexpr Revision kMaximumSqliteRevision =
    static_cast<Revision>(std::numeric_limits<sqlite3_int64>::max());

constexpr std::string_view kSchemaV1 = R"sql(
CREATE TABLE project_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  project_uuid TEXT NOT NULL CHECK (length(project_uuid) > 0),
  schema_version INTEGER NOT NULL CHECK (schema_version >= 1),
  head_revision INTEGER NOT NULL CHECK (head_revision >= 0),
  saved_revision INTEGER NOT NULL CHECK (
    saved_revision >= 0 AND saved_revision <= head_revision
  ),
  clean_close INTEGER NOT NULL CHECK (clean_close IN (0, 1)),
  heartbeat_utc_ms INTEGER NOT NULL CHECK (heartbeat_utc_ms >= 0)
);

CREATE TABLE command_journal (
  revision INTEGER PRIMARY KEY CHECK (revision > 0),
  command_type TEXT NOT NULL CHECK (length(command_type) > 0),
  payload_kind INTEGER NOT NULL CHECK (payload_kind IN (0, 1)),
  payload_text TEXT,
  payload_blob BLOB,
  created_at_utc_ms INTEGER NOT NULL CHECK (created_at_utc_ms >= 0),
  CHECK (
    (payload_kind = 0 AND payload_text IS NOT NULL AND payload_blob IS NULL) OR
    (payload_kind = 1 AND payload_text IS NULL AND payload_blob IS NOT NULL)
  )
);

CREATE TABLE schema_migrations (
  version INTEGER PRIMARY KEY CHECK (version > 0),
  name TEXT NOT NULL,
  applied_at_utc_ms INTEGER NOT NULL CHECK (applied_at_utc_ms >= 0)
);
)sql";

struct Migration {
  std::uint32_t from_version;
  std::uint32_t to_version;
  std::string_view name;
  std::string_view sql;
};

// Future migrations are appended here as adjacent, forward-only steps. The
// runner wraps each entry, its migration record, metadata version, and
// user_version update in one exclusive transaction.
constexpr std::array<Migration, 1> kMigrations{{
    {0, 1, "create_project_store_v1", kSchemaV1},
}};

[[noreturn]] void throw_sqlite(sqlite3* db, const std::string_view context, const int result) {
  std::string message(context);
  message.append(": ");
  message.append(sqlite3_errmsg(db));
  throw SqliteError(std::move(message), result, sqlite3_extended_errcode(db));
}

void require_sqlite(const int result, sqlite3* db, const std::string_view context) {
  if (result != SQLITE_OK) {
    throw_sqlite(db, context, result);
  }
}

class Statement final {
public:
  Statement(sqlite3* db, const std::string_view sql) : db_(db) {
    const int result = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()),
                                          SQLITE_PREPARE_PERSISTENT, &statement_, nullptr);
    require_sqlite(result, db_, "Failed to prepare SQLite statement");
  }

  ~Statement() noexcept {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind_int64(const int index, const sqlite3_int64 value) {
    require_sqlite(sqlite3_bind_int64(statement_, index, value), db_,
                   "Failed to bind SQLite integer");
  }

  void bind_int(const int index, const int value) {
    require_sqlite(sqlite3_bind_int(statement_, index, value), db_,
                   "Failed to bind SQLite integer");
  }

  void bind_text(const int index, const std::string_view value) {
    require_sqlite(sqlite3_bind_text64(statement_, index, value.data(),
                                       static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT,
                                       SQLITE_UTF8),
                   db_, "Failed to bind SQLite text");
  }

  void bind_blob(const int index, const std::span<const std::byte> value) {
    static constexpr std::byte kEmptyBlobStorage{0};
    const void* data = value.empty() ? &kEmptyBlobStorage : value.data();
    require_sqlite(sqlite3_bind_blob64(statement_, index, data,
                                       static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT),
                   db_, "Failed to bind SQLite blob");
  }

  void bind_null(const int index) {
    require_sqlite(sqlite3_bind_null(statement_, index), db_, "Failed to bind SQLite null");
  }

  int step() {
    const int result = sqlite3_step(statement_);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
      throw_sqlite(db_, "Failed to step SQLite statement", result);
    }
    return result;
  }

  void reset() {
    require_sqlite(sqlite3_reset(statement_), db_, "Failed to reset SQLite statement");
    require_sqlite(sqlite3_clear_bindings(statement_), db_,
                   "Failed to clear SQLite statement bindings");
  }

  [[nodiscard]] sqlite3_int64 column_int64(const int index) const {
    return sqlite3_column_int64(statement_, index);
  }

  [[nodiscard]] int column_int(const int index) const {
    return sqlite3_column_int(statement_, index);
  }

  [[nodiscard]] std::string column_text(const int index) const {
    const auto* value = sqlite3_column_text(statement_, index);
    const int length = sqlite3_column_bytes(statement_, index);
    if (value == nullptr) {
      throw ProjectStoreError("Project database contains an unexpected null text value");
    }
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(length)};
  }

  [[nodiscard]] BinaryPayload column_blob(const int index) const {
    const void* value = sqlite3_column_blob(statement_, index);
    const int length = sqlite3_column_bytes(statement_, index);
    if (length < 0 || (length > 0 && value == nullptr)) {
      throw ProjectStoreError("Project database contains an invalid blob value");
    }
    const auto* begin = static_cast<const std::byte*>(value);
    if (length == 0) {
      return {};
    }
    return {begin, begin + static_cast<std::ptrdiff_t>(length)};
  }

private:
  sqlite3* db_;
  sqlite3_stmt* statement_ = nullptr;
};

class Transaction final {
public:
  Transaction(SqliteConnection& connection, const std::string_view begin_sql)
      : connection_(connection) {
    connection_.execute(begin_sql);
  }

  ~Transaction() noexcept {
    if (!committed_) {
      try {
        connection_.execute("ROLLBACK");
      } catch (...) {
      }
    }
  }

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void commit() {
    connection_.execute("COMMIT");
    committed_ = true;
  }

private:
  SqliteConnection& connection_;
  bool committed_ = false;
};

class TemporaryDatabase final {
public:
  explicit TemporaryDatabase(std::filesystem::path path) : path_(std::move(path)) {}

  ~TemporaryDatabase() noexcept {
    if (!keep_) {
      remove_path(path_);
      auto wal = path_;
      wal += "-wal";
      remove_path(wal);
      auto shm = path_;
      shm += "-shm";
      remove_path(shm);
      auto journal = path_;
      journal += "-journal";
      remove_path(journal);
    }
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

  void release() noexcept {
    keep_ = true;
  }

private:
  static void remove_path(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  std::filesystem::path path_;
  bool keep_ = false;
};

void configure_working_database(SqliteConnection& connection) {
  connection.execute("PRAGMA foreign_keys = ON");
  connection.execute("PRAGMA journal_mode = WAL");
  connection.execute("PRAGMA synchronous = FULL");

  Statement foreign_keys(connection.native_handle(), "PRAGMA foreign_keys");
  if (foreign_keys.step() != SQLITE_ROW || foreign_keys.column_int(0) != 1) {
    throw ProjectStoreError("SQLite foreign-key enforcement could not be enabled");
  }

  Statement journal_mode(connection.native_handle(), "PRAGMA journal_mode");
  if (journal_mode.step() != SQLITE_ROW || journal_mode.column_text(0) != "wal") {
    throw ProjectStoreError("SQLite WAL mode could not be enabled for the working database");
  }

  Statement synchronous(connection.native_handle(), "PRAGMA synchronous");
  if (synchronous.step() != SQLITE_ROW || synchronous.column_int(0) != 2) {
    throw ProjectStoreError("SQLite synchronous=FULL could not be enabled");
  }
}

std::int64_t now_utc_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string generate_uuid_v7() {
  std::array<std::uint8_t, 16> bytes{};
  std::random_device random;
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(random());
  }

  const auto timestamp = static_cast<std::uint64_t>(now_utc_ms());
  bytes[0] = static_cast<std::uint8_t>((timestamp >> 40U) & 0xffU);
  bytes[1] = static_cast<std::uint8_t>((timestamp >> 32U) & 0xffU);
  bytes[2] = static_cast<std::uint8_t>((timestamp >> 24U) & 0xffU);
  bytes[3] = static_cast<std::uint8_t>((timestamp >> 16U) & 0xffU);
  bytes[4] = static_cast<std::uint8_t>((timestamp >> 8U) & 0xffU);
  bytes[5] = static_cast<std::uint8_t>(timestamp & 0xffU);
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x70U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  constexpr char kHex[] = "0123456789abcdef";
  std::string uuid;
  uuid.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      uuid.push_back('-');
    }
    uuid.push_back(kHex[bytes[index] >> 4U]);
    uuid.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return uuid;
}

sqlite3_int64 to_sql_revision(const Revision revision) {
  if (revision > kMaximumSqliteRevision) {
    throw std::out_of_range("Revision exceeds SQLite's signed 64-bit integer range");
  }
  return static_cast<sqlite3_int64>(revision);
}

Revision from_sql_revision(const sqlite3_int64 revision) {
  if (revision < 0) {
    throw ProjectStoreError("Project database contains a negative revision");
  }
  return static_cast<Revision>(revision);
}

std::uint32_t read_user_version(sqlite3* db) {
  Statement statement(db, "PRAGMA user_version");
  if (statement.step() != SQLITE_ROW) {
    throw ProjectStoreError("SQLite did not return a project schema version");
  }
  const sqlite3_int64 version = statement.column_int64(0);
  if (version < 0 ||
      version > static_cast<sqlite3_int64>(std::numeric_limits<std::uint32_t>::max())) {
    throw ProjectStoreError("Project database has an invalid schema version");
  }
  return static_cast<std::uint32_t>(version);
}

sqlite3_int64 count_user_tables(sqlite3* db) {
  Statement statement(db, "SELECT count(*) FROM sqlite_schema "
                          "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
  if (statement.step() != SQLITE_ROW) {
    throw ProjectStoreError("Could not inspect the project database schema");
  }
  return statement.column_int64(0);
}

void insert_migration_record(sqlite3* db, const Migration& migration) {
  Statement statement(db, "INSERT INTO schema_migrations(version, name, applied_at_utc_ms) "
                          "VALUES(?, ?, ?)");
  statement.bind_int64(1, static_cast<sqlite3_int64>(migration.to_version));
  statement.bind_text(2, migration.name);
  statement.bind_int64(3, now_utc_ms());
  if (statement.step() != SQLITE_DONE) {
    throw ProjectStoreError("Could not record the project schema migration");
  }
}

void insert_initial_metadata(sqlite3* db, std::string_view project_uuid);
void preserve_pre_migration_backup(SqliteConnection& connection, std::uint32_t from_version);

void update_metadata_schema_version(sqlite3* db, const std::uint32_t version) {
  Statement statement(db, "UPDATE project_metadata SET schema_version = ? WHERE singleton = 1");
  statement.bind_int64(1, static_cast<sqlite3_int64>(version));
  if (statement.step() != SQLITE_DONE) {
    throw ProjectStoreError("Could not update the project metadata schema version");
  }
}

void run_migrations(SqliteConnection& connection, const std::string_view initial_project_uuid) {
  std::uint32_t version = read_user_version(connection.native_handle());
  if (version > kCurrentProjectSchemaVersion) {
    std::ostringstream message;
    message << "Project schema version " << version << " is newer than the supported version "
            << kCurrentProjectSchemaVersion;
    throw ProjectStoreError(message.str());
  }

  if (version == 0 && count_user_tables(connection.native_handle()) != 0) {
    throw ProjectStoreError("A version-zero database containing unknown tables cannot be migrated");
  }

  // Version zero is an empty SQLite file, not a prior project format. Every
  // migration of an established project is preserved before any schema write.
  if (version > 0 && version < kCurrentProjectSchemaVersion) {
    preserve_pre_migration_backup(connection, version);
  }

  while (version < kCurrentProjectSchemaVersion) {
    const Migration* selected = nullptr;
    for (const auto& migration : kMigrations) {
      if (migration.from_version == version) {
        selected = &migration;
        break;
      }
    }
    if (selected == nullptr || selected->to_version != version + 1U) {
      throw ProjectStoreError("No forward migration exists for this project schema");
    }

    Transaction transaction(connection, "BEGIN EXCLUSIVE");
    connection.execute(selected->sql);
    if (selected->from_version == 0) {
      insert_initial_metadata(connection.native_handle(), initial_project_uuid);
    }
    update_metadata_schema_version(connection.native_handle(), selected->to_version);
    insert_migration_record(connection.native_handle(), *selected);

    const std::string set_user_version =
        "PRAGMA user_version = " + std::to_string(selected->to_version);
    connection.execute(set_user_version);
    transaction.commit();

    version = selected->to_version;
  }
}

void validate_schema(sqlite3* db) {
  constexpr std::array<std::string_view, 3> kRequiredTables{"project_metadata", "command_journal",
                                                            "schema_migrations"};
  Statement statement(db, "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = ?");
  for (const auto table : kRequiredTables) {
    statement.bind_text(1, table);
    if (statement.step() != SQLITE_ROW || statement.column_int64(0) != 1) {
      throw ProjectStoreError("Project schema is missing required table: " + std::string(table));
    }
    statement.reset();
  }
}

ProjectMetadata read_metadata(sqlite3* db) {
  Statement statement(db, "SELECT project_uuid, schema_version, head_revision, saved_revision, "
                          "clean_close, heartbeat_utc_ms FROM project_metadata "
                          "WHERE singleton = 1");
  if (statement.step() != SQLITE_ROW) {
    throw ProjectStoreError("Project database has no metadata record");
  }

  ProjectMetadata metadata;
  metadata.project_uuid = statement.column_text(0);
  const sqlite3_int64 schema_version = statement.column_int64(1);
  if (schema_version < 0 ||
      schema_version > static_cast<sqlite3_int64>(std::numeric_limits<std::uint32_t>::max())) {
    throw ProjectStoreError("Project metadata has an invalid schema version");
  }
  metadata.schema_version = static_cast<std::uint32_t>(schema_version);
  metadata.head_revision = from_sql_revision(statement.column_int64(2));
  metadata.saved_revision = from_sql_revision(statement.column_int64(3));
  const int clean_close = statement.column_int(4);
  if (clean_close != 0 && clean_close != 1) {
    throw ProjectStoreError("Project metadata has an invalid clean-close state");
  }
  metadata.clean_close = clean_close != 0;
  metadata.heartbeat_utc_ms = statement.column_int64(5);

  if (metadata.project_uuid.empty()) {
    throw ProjectStoreError("Project metadata has an empty project UUID");
  }
  if (metadata.saved_revision > metadata.head_revision) {
    throw ProjectStoreError("Saved revision is newer than the project head");
  }
  if (metadata.heartbeat_utc_ms < 0) {
    throw ProjectStoreError("Project metadata has a negative heartbeat");
  }
  return metadata;
}

void insert_initial_metadata(sqlite3* db, const std::string_view project_uuid) {
  if (project_uuid.empty()) {
    throw std::invalid_argument("Project UUID must not be empty");
  }
  Statement statement(db, "INSERT INTO project_metadata("
                          "singleton, project_uuid, schema_version, head_revision, saved_revision, "
                          "clean_close, heartbeat_utc_ms) VALUES(1, ?, ?, 0, 0, 1, ?)");
  statement.bind_text(1, project_uuid);
  statement.bind_int64(2, kCurrentProjectSchemaVersion);
  statement.bind_int64(3, now_utc_ms());
  if (statement.step() != SQLITE_DONE) {
    throw ProjectStoreError("Could not create initial project metadata");
  }
}

void ensure_single_metadata_record(sqlite3* db) {
  Statement statement(db, "SELECT count(*) FROM project_metadata");
  if (statement.step() != SQLITE_ROW || statement.column_int64(0) != 1) {
    throw ProjectStoreError("Project database must contain exactly one metadata record");
  }
}

void ensure_expected_revision(const Revision expected, const Revision actual) {
  if (expected != actual) {
    throw RevisionConflict(expected, actual);
  }
}

void update_open_state(sqlite3* db) {
  Statement statement(db, "UPDATE project_metadata SET clean_close = 0, heartbeat_utc_ms = ? "
                          "WHERE singleton = 1");
  statement.bind_int64(1, now_utc_ms());
  if (statement.step() != SQLITE_DONE || sqlite3_changes(db) != 1) {
    throw ProjectStoreError("Could not mark the project database as open");
  }
}

IntegrityResult quick_check_database(sqlite3* db) {
  IntegrityResult result;
  Statement statement(db, "PRAGMA quick_check");
  while (statement.step() == SQLITE_ROW) {
    result.messages.push_back(statement.column_text(0));
  }
  if (result.messages.empty()) {
    result.messages.emplace_back("SQLite quick_check returned no result");
  }
  return result;
}

IntegrityResult bounded_quick_check_database(sqlite3* db) {
  IntegrityResult result;
  Statement statement(db, "PRAGMA quick_check(1)");
  if (statement.step() == SQLITE_ROW) {
    result.messages.push_back(statement.column_text(0));
  }
  if (result.messages.empty()) {
    result.messages.emplace_back("SQLite quick_check returned no result");
  }
  return result;
}

std::string integrity_error_message(const IntegrityResult& result) {
  std::string message = "Project database integrity check failed";
  for (const auto& detail : result.messages) {
    message.append("; ");
    message.append(detail);
  }
  return message;
}

std::string bounded_recovery_diagnostic(std::string message) {
  constexpr std::size_t kMaximumDiagnosticBytes = 1'024;
  if (message.size() <= kMaximumDiagnosticBytes) {
    return message;
  }
  message.resize(kMaximumDiagnosticBytes - 3U);
  message.append("...");
  return message;
}

auto recovery_path_key(const std::filesystem::path& path) {
  return path.generic_u8string();
}

bool has_working_database_suffix(const std::filesystem::path& path) {
  constexpr std::u8string_view kSuffix = u8".working.sqlite";
  const auto name = path.filename().generic_u8string();
  return name.size() >= kSuffix.size() && name.ends_with(kSuffix);
}

void validate_journal_head(sqlite3* db, const ProjectMetadata& metadata) {
  Statement statement(db, "SELECT count(*), coalesce(max(revision), 0) FROM command_journal");
  if (statement.step() != SQLITE_ROW) {
    throw ProjectStoreError("Could not validate the command journal");
  }
  const sqlite3_int64 entry_count = statement.column_int64(0);
  const Revision maximum_revision = from_sql_revision(statement.column_int64(1));
  if (entry_count < 0 || static_cast<Revision>(entry_count) != metadata.head_revision ||
      maximum_revision != metadata.head_revision) {
    throw ProjectStoreError("Command journal does not match the project head revision");
  }
}

RecoveryCandidate inspect_recovery_candidate(const std::filesystem::path& path,
                                             const int busy_timeout_ms) {
  RecoveryCandidate candidate;
  candidate.working_database = path;

  try {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error) {
      throw ProjectStoreError("Could not inspect recovery file type: " + status_error.message());
    }
    if (!std::filesystem::is_regular_file(status)) {
      throw ProjectStoreError("Recovery candidate is not a regular file");
    }

    SqliteConnection connection(path, SqliteOpenMode::kReadOnly, busy_timeout_ms);
    if (sqlite3_db_readonly(connection.native_handle(), "main") != 1) {
      throw ProjectStoreError("SQLite did not open the recovery database read-only");
    }

    const IntegrityResult integrity = bounded_quick_check_database(connection.native_handle());
    candidate.integrity_ok = integrity.ok();
    if (!candidate.integrity_ok) {
      candidate.diagnostic = bounded_recovery_diagnostic(integrity_error_message(integrity));
      return candidate;
    }

    const std::uint32_t user_version = read_user_version(connection.native_handle());
    if (user_version != kCurrentProjectSchemaVersion) {
      throw ProjectStoreError("Unsupported recovery database schema version " +
                              std::to_string(user_version));
    }
    validate_schema(connection.native_handle());
    ensure_single_metadata_record(connection.native_handle());
    const ProjectMetadata metadata = read_metadata(connection.native_handle());
    if (metadata.schema_version != user_version) {
      throw ProjectStoreError("Project metadata schema version does not match SQLite user_version");
    }
    validate_journal_head(connection.native_handle(), metadata);

    candidate.project_uuid = metadata.project_uuid;
    candidate.schema_version = metadata.schema_version;
    candidate.head_revision = metadata.head_revision;
    candidate.saved_revision = metadata.saved_revision;
    candidate.clean_close = metadata.clean_close;
    candidate.heartbeat_utc_ms = metadata.heartbeat_utc_ms;
    candidate.valid_project_database = true;
    candidate.recovery_recommended =
        !metadata.clean_close || metadata.head_revision != metadata.saved_revision;
  } catch (const std::exception& error) {
    candidate.diagnostic = bounded_recovery_diagnostic("Could not inspect recovery database: " +
                                                       std::string(error.what()));
  }

  return candidate;
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    throw ProjectStoreError("Could not resolve project path: " + error.message());
  }
  return absolute.lexically_normal();
}

void ensure_distinct_paths(const std::filesystem::path& first,
                           const std::filesystem::path& second) {
  const auto normalized_first = normalized_absolute(first);
  const auto normalized_second = normalized_absolute(second);
  if (normalized_first == normalized_second) {
    throw ProjectStoreError("Checkpoint destination cannot be the open working database");
  }
  std::error_code error;
  if (std::filesystem::exists(second, error) && !error &&
      std::filesystem::equivalent(first, second, error) && !error) {
    throw ProjectStoreError("Checkpoint destination cannot refer to the open working database");
  }
}

void ensure_no_live_sqlite_sidecars(const std::filesystem::path& path) {
  for (const std::string_view suffix : {"-wal", "-shm", "-journal"}) {
    auto sidecar = path;
    sidecar += suffix;
    std::error_code error;
    const bool exists = std::filesystem::exists(sidecar, error);
    if (error) {
      throw ProjectStoreError("Could not inspect checkpoint sidecar files: " + error.message());
    }
    if (exists) {
      throw ProjectStoreError(
          "Checkpoint destination has live or unrecovered SQLite sidecar files");
    }
  }
}

void copy_database(sqlite3* source, sqlite3* destination) {
  sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
  if (backup == nullptr) {
    throw_sqlite(destination, "Could not initialize SQLite checkpoint backup",
                 sqlite3_errcode(destination));
  }

  int step_result = SQLITE_OK;
  int busy_attempts = 0;
  while (step_result != SQLITE_DONE) {
    step_result = sqlite3_backup_step(backup, 256);
    if (step_result == SQLITE_OK) {
      continue;
    }
    if (step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
      if (++busy_attempts > 500) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    break;
  }

  const int finish_result = sqlite3_backup_finish(backup);
  if (step_result != SQLITE_DONE) {
    throw_sqlite(destination, "SQLite checkpoint backup did not complete", step_result);
  }
  require_sqlite(finish_result, destination, "Could not finish SQLite checkpoint backup");
}

#if defined(_WIN32)
std::string windows_error_message(const DWORD error_code) {
  char* buffer = nullptr;
  const DWORD size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string result =
      size == 0 ? "Windows error " + std::to_string(error_code) : std::string(buffer, size);
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return result;
}

void sync_file(const std::filesystem::path& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw ProjectStoreError("Could not open checkpoint for sync: " +
                            windows_error_message(GetLastError()));
  }
  if (!FlushFileBuffers(file)) {
    const DWORD error = GetLastError();
    CloseHandle(file);
    throw ProjectStoreError("Could not sync checkpoint: " + windows_error_message(error));
  }
  CloseHandle(file);
}

void atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw ProjectStoreError("Could not atomically replace checkpoint: " +
                            windows_error_message(GetLastError()));
  }
}
#else
void sync_file(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    throw ProjectStoreError("Could not open checkpoint for fsync: " +
                            std::string(std::strerror(errno)));
  }
  int result;
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    throw ProjectStoreError("Could not fsync checkpoint: " +
                            std::string(std::strerror(saved_errno)));
  }
}

void sync_directory(const std::filesystem::path& directory) {
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    throw ProjectStoreError("Could not open checkpoint directory for fsync: " +
                            std::string(std::strerror(errno)));
  }
  int result;
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    throw ProjectStoreError("Could not fsync checkpoint directory: " +
                            std::string(std::strerror(saved_errno)));
  }
}

void atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (::rename(source.c_str(), destination.c_str()) != 0) {
    throw ProjectStoreError("Could not atomically replace checkpoint: " +
                            std::string(std::strerror(errno)));
  }
  sync_directory(destination.parent_path());
}
#endif

void preserve_pre_migration_backup(SqliteConnection& connection, const std::uint32_t from_version) {
  auto backup_path = normalized_absolute(connection.path());
  backup_path += ".pre-migration-v";
  backup_path += std::to_string(from_version);
  backup_path += ".bak";

  auto temporary_path = backup_path;
  temporary_path += ".tmp-";
  temporary_path += generate_uuid_v7();
  TemporaryDatabase temporary(temporary_path);

  {
    Transaction snapshot(connection, "BEGIN");
    SqliteConnection backup_connection(temporary.path(), SqliteOpenMode::kReadWriteCreate);
    copy_database(connection.native_handle(), backup_connection.native_handle());
    snapshot.commit();

    backup_connection.execute("PRAGMA journal_mode = DELETE");
    backup_connection.execute("PRAGMA synchronous = FULL");
    const IntegrityResult integrity = quick_check_database(backup_connection.native_handle());
    if (!integrity.ok()) {
      throw ProjectStoreError(integrity_error_message(integrity));
    }
  }

  sync_file(temporary.path());
  atomic_replace(temporary.path(), backup_path);
  temporary.release();
}

void mark_checkpoint_metadata(sqlite3* db, const Revision revision) {
  Statement statement(db, "UPDATE project_metadata SET saved_revision = ?, clean_close = 1, "
                          "heartbeat_utc_ms = ? WHERE singleton = 1 AND head_revision = ?");
  statement.bind_int64(1, to_sql_revision(revision));
  statement.bind_int64(2, now_utc_ms());
  statement.bind_int64(3, to_sql_revision(revision));
  if (statement.step() != SQLITE_DONE || sqlite3_changes(db) != 1) {
    throw ProjectStoreError("Checkpoint snapshot does not match its requested revision");
  }
}

} // namespace

RevisionConflict::RevisionConflict(const Revision expected, const Revision actual)
    : ProjectStoreError("Expected project revision " + std::to_string(expected) + " but head is " +
                        std::to_string(actual)),
      expected_(expected), actual_(actual) {}

bool IntegrityResult::ok() const noexcept {
  return messages.size() == 1 && messages.front() == "ok";
}

RecoveryCatalog scan_recovery_directory(const std::filesystem::path& directory,
                                        const RecoveryScanOptions options) {
  if (directory.empty()) {
    throw std::invalid_argument("Recovery directory must not be empty");
  }
  if (options.maximum_candidates == 0) {
    throw std::invalid_argument("Recovery candidate limit must be greater than zero");
  }
  if (options.busy_timeout_ms < 0) {
    throw std::invalid_argument("Recovery scan busy timeout must not be negative");
  }

  RecoveryCatalog catalog;
  std::error_code filesystem_error;
  const bool directory_exists = std::filesystem::exists(directory, filesystem_error);
  if (filesystem_error) {
    throw ProjectStoreError("Could not inspect recovery directory: " + filesystem_error.message());
  }
  if (!directory_exists) {
    return catalog;
  }
  if (!std::filesystem::is_directory(directory, filesystem_error) || filesystem_error) {
    throw ProjectStoreError("Recovery path is not an accessible directory");
  }

  std::vector<std::filesystem::path> paths;
  paths.reserve(options.maximum_candidates);
  std::filesystem::directory_iterator iterator(directory, filesystem_error);
  if (filesystem_error) {
    throw ProjectStoreError("Could not enumerate recovery directory: " +
                            filesystem_error.message());
  }

  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto& entry = *iterator;
    if (has_working_database_suffix(entry.path())) {
      std::error_code status_error;
      const auto status = entry.symlink_status(status_error);
      if (status_error || std::filesystem::is_regular_file(status)) {
        paths.push_back(entry.path());
        if (paths.size() > options.maximum_candidates) {
          const auto last =
              std::max_element(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
                return recovery_path_key(left) < recovery_path_key(right);
              });
          paths.erase(last);
          catalog.truncated = true;
        }
      }
    }

    iterator.increment(filesystem_error);
    if (filesystem_error) {
      throw ProjectStoreError("Could not enumerate recovery directory: " +
                              filesystem_error.message());
    }
  }

  std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
    return recovery_path_key(left) < recovery_path_key(right);
  });
  catalog.candidates.reserve(paths.size());
  for (const auto& path : paths) {
    catalog.candidates.push_back(inspect_recovery_candidate(path, options.busy_timeout_ms));
  }

  std::sort(catalog.candidates.begin(), catalog.candidates.end(),
            [](const RecoveryCandidate& left, const RecoveryCandidate& right) {
              if (left.recovery_recommended != right.recovery_recommended) {
                return left.recovery_recommended;
              }
              if (left.heartbeat_utc_ms != right.heartbeat_utc_ms) {
                return left.heartbeat_utc_ms > right.heartbeat_utc_ms;
              }
              return recovery_path_key(left.working_database) <
                     recovery_path_key(right.working_database);
            });
  return catalog;
}

ProjectStore::ProjectStore(std::filesystem::path working_database, OpenOptions options)
    : connection_(std::move(working_database),
                  options.create_if_missing ? SqliteOpenMode::kReadWriteCreate
                                            : SqliteOpenMode::kReadWrite,
                  options.busy_timeout_ms) {
  initialize(std::move(options));
}

void ProjectStore::initialize(OpenOptions options) {
  if (options.project_uuid.has_value() && options.project_uuid->empty()) {
    throw std::invalid_argument("Project UUID must not be empty");
  }
  configure_working_database(connection_);

  IntegrityResult opening_integrity;
  if (options.run_integrity_check) {
    opening_integrity = quick_check();
    if (!opening_integrity.ok()) {
      throw ProjectStoreError(integrity_error_message(opening_integrity));
    }
  } else {
    opening_integrity.messages.emplace_back("not checked");
  }

  const std::string initial_project_uuid = options.project_uuid.value_or(generate_uuid_v7());
  run_migrations(connection_, initial_project_uuid);
  validate_schema(connection_.native_handle());
  ensure_single_metadata_record(connection_.native_handle());

  const ProjectMetadata persisted = read_metadata(connection_.native_handle());
  if (persisted.schema_version != kCurrentProjectSchemaVersion) {
    throw ProjectStoreError("Project metadata schema version does not match SQLite user_version");
  }
  if (options.project_uuid.has_value() && *options.project_uuid != persisted.project_uuid) {
    throw ProjectStoreError("Existing project UUID does not match the requested project UUID");
  }

  recovery_status_.integrity_checked = options.run_integrity_check;
  recovery_status_.integrity_ok = options.run_integrity_check && opening_integrity.ok();
  recovery_status_.previous_clean_close = persisted.clean_close;
  recovery_status_.had_unsaved_changes = persisted.head_revision != persisted.saved_revision;
  recovery_status_.recovery_recommended =
      !persisted.clean_close || recovery_status_.had_unsaved_changes;
  recovery_status_.head_revision = persisted.head_revision;
  recovery_status_.saved_revision = persisted.saved_revision;
  recovery_status_.last_heartbeat_utc_ms = persisted.heartbeat_utc_ms;
  recovery_status_.integrity_messages = std::move(opening_integrity.messages);

  Transaction transaction(connection_, "BEGIN IMMEDIATE");
  update_open_state(connection_.native_handle());
  transaction.commit();
}

ProjectMetadata ProjectStore::metadata() const {
  return read_metadata(connection_.native_handle());
}

IntegrityResult ProjectStore::quick_check() const {
  return quick_check_database(connection_.native_handle());
}

Revision ProjectStore::append_command(const std::string_view command_type,
                                      const std::string_view payload,
                                      const Revision expected_revision) {
  return append_payload(command_type, CommandPayload{std::string(payload)}, expected_revision);
}

Revision ProjectStore::append_command(const std::string_view command_type,
                                      const std::span<const std::byte> payload,
                                      const Revision expected_revision) {
  return append_payload(command_type, CommandPayload{BinaryPayload(payload.begin(), payload.end())},
                        expected_revision);
}

Revision ProjectStore::append_payload(const std::string_view command_type,
                                      const CommandPayload& payload,
                                      const Revision expected_revision) {
  if (command_type.empty()) {
    throw std::invalid_argument("Command type must not be empty");
  }
  to_sql_revision(expected_revision);

  Transaction transaction(connection_, "BEGIN IMMEDIATE");
  const ProjectMetadata before = read_metadata(connection_.native_handle());
  ensure_expected_revision(expected_revision, before.head_revision);
  if (before.head_revision == kMaximumSqliteRevision) {
    throw std::overflow_error("Project revision space is exhausted");
  }

  const Revision next_revision = before.head_revision + 1U;
  const std::int64_t timestamp = now_utc_ms();
  Statement insert(connection_.native_handle(),
                   "INSERT INTO command_journal(revision, command_type, payload_kind, "
                   "payload_text, payload_blob, created_at_utc_ms) VALUES(?, ?, ?, ?, ?, ?)");
  insert.bind_int64(1, to_sql_revision(next_revision));
  insert.bind_text(2, command_type);
  if (std::holds_alternative<std::string>(payload)) {
    insert.bind_int(3, 0);
    insert.bind_text(4, std::get<std::string>(payload));
    insert.bind_null(5);
  } else {
    insert.bind_int(3, 1);
    insert.bind_null(4);
    insert.bind_blob(5, std::get<BinaryPayload>(payload));
  }
  insert.bind_int64(6, timestamp);
  if (insert.step() != SQLITE_DONE) {
    throw ProjectStoreError("Could not append the command journal entry");
  }

  Statement update(connection_.native_handle(),
                   "UPDATE project_metadata SET head_revision = ?, clean_close = 0, "
                   "heartbeat_utc_ms = ? WHERE singleton = 1 AND head_revision = ?");
  update.bind_int64(1, to_sql_revision(next_revision));
  update.bind_int64(2, timestamp);
  update.bind_int64(3, to_sql_revision(before.head_revision));
  if (update.step() != SQLITE_DONE || sqlite3_changes(connection_.native_handle()) != 1) {
    throw ProjectStoreError("Could not advance the project head revision");
  }

  transaction.commit();
  return next_revision;
}

std::vector<JournalEntry> ProjectStore::read_commands(const Revision after_revision) const {
  Statement statement(connection_.native_handle(),
                      "SELECT revision, command_type, payload_kind, payload_text, payload_blob, "
                      "created_at_utc_ms FROM command_journal WHERE revision > ? "
                      "ORDER BY revision ASC");
  statement.bind_int64(1, to_sql_revision(after_revision));

  std::vector<JournalEntry> entries;
  while (statement.step() == SQLITE_ROW) {
    JournalEntry entry;
    entry.revision = from_sql_revision(statement.column_int64(0));
    entry.command_type = statement.column_text(1);
    const int payload_kind = statement.column_int(2);
    if (payload_kind == 0) {
      entry.payload = statement.column_text(3);
    } else if (payload_kind == 1) {
      entry.payload = statement.column_blob(4);
    } else {
      throw ProjectStoreError("Command journal has an unknown payload kind");
    }
    entry.created_at_utc_ms = statement.column_int64(5);
    entries.push_back(std::move(entry));
  }
  return entries;
}

void ProjectStore::mark_saved(const Revision expected_revision) {
  Transaction transaction(connection_, "BEGIN IMMEDIATE");
  const ProjectMetadata current = read_metadata(connection_.native_handle());
  ensure_expected_revision(expected_revision, current.head_revision);

  Statement statement(connection_.native_handle(),
                      "UPDATE project_metadata SET saved_revision = ?, heartbeat_utc_ms = ? "
                      "WHERE singleton = 1 AND head_revision = ?");
  statement.bind_int64(1, to_sql_revision(expected_revision));
  statement.bind_int64(2, now_utc_ms());
  statement.bind_int64(3, to_sql_revision(expected_revision));
  if (statement.step() != SQLITE_DONE || sqlite3_changes(connection_.native_handle()) != 1) {
    throw ProjectStoreError("Could not update the saved project revision");
  }
  transaction.commit();
}

void ProjectStore::update_heartbeat() {
  Transaction transaction(connection_, "BEGIN IMMEDIATE");
  Statement statement(connection_.native_handle(),
                      "UPDATE project_metadata SET heartbeat_utc_ms = ? WHERE singleton = 1");
  statement.bind_int64(1, now_utc_ms());
  if (statement.step() != SQLITE_DONE || sqlite3_changes(connection_.native_handle()) != 1) {
    throw ProjectStoreError("Could not update the project heartbeat");
  }
  transaction.commit();
}

void ProjectStore::mark_clean_close(const Revision expected_revision) {
  Transaction transaction(connection_, "BEGIN IMMEDIATE");
  const ProjectMetadata current = read_metadata(connection_.native_handle());
  ensure_expected_revision(expected_revision, current.head_revision);

  Statement statement(connection_.native_handle(),
                      "UPDATE project_metadata SET clean_close = 1, heartbeat_utc_ms = ? "
                      "WHERE singleton = 1 AND head_revision = ?");
  statement.bind_int64(1, now_utc_ms());
  statement.bind_int64(2, to_sql_revision(expected_revision));
  if (statement.step() != SQLITE_DONE || sqlite3_changes(connection_.native_handle()) != 1) {
    throw ProjectStoreError("Could not mark a clean project close");
  }
  transaction.commit();
}

Revision ProjectStore::checkpoint_to(const std::filesystem::path& destination,
                                     const Revision expected_revision) {
  if (destination.empty()) {
    throw std::invalid_argument("Checkpoint destination must not be empty");
  }
  to_sql_revision(expected_revision);
  ensure_distinct_paths(connection_.path(), destination);

  const std::filesystem::path absolute_destination = normalized_absolute(destination);
  const std::filesystem::path parent = absolute_destination.parent_path();
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(parent, filesystem_error) || filesystem_error) {
    throw ProjectStoreError("Checkpoint destination directory does not exist or is inaccessible");
  }
  if (std::filesystem::is_directory(absolute_destination, filesystem_error) && !filesystem_error) {
    throw ProjectStoreError("Checkpoint destination is a directory");
  }
  ensure_no_live_sqlite_sidecars(absolute_destination);

  auto temporary_path = absolute_destination;
  temporary_path += ".tmp-";
  temporary_path += generate_uuid_v7();
  TemporaryDatabase temporary(temporary_path);

  {
    Transaction snapshot(connection_, "BEGIN");
    const ProjectMetadata source_metadata = read_metadata(connection_.native_handle());
    ensure_expected_revision(expected_revision, source_metadata.head_revision);

    SqliteConnection destination_connection(temporary.path(), SqliteOpenMode::kReadWriteCreate);
    copy_database(connection_.native_handle(), destination_connection.native_handle());
    snapshot.commit();

    destination_connection.execute("PRAGMA journal_mode = DELETE");
    destination_connection.execute("PRAGMA synchronous = FULL");
    Transaction destination_update(destination_connection, "BEGIN IMMEDIATE");
    mark_checkpoint_metadata(destination_connection.native_handle(), expected_revision);
    destination_update.commit();

    const IntegrityResult checkpoint_integrity =
        quick_check_database(destination_connection.native_handle());
    if (!checkpoint_integrity.ok()) {
      throw ProjectStoreError(integrity_error_message(checkpoint_integrity));
    }
  }

  sync_file(temporary.path());
  atomic_replace(temporary.path(), absolute_destination);
  temporary.release();

  {
    Transaction update_source(connection_, "BEGIN IMMEDIATE");
    const ProjectMetadata current = read_metadata(connection_.native_handle());
    if (current.head_revision < expected_revision) {
      throw ProjectStoreError("Project head moved behind the completed checkpoint revision");
    }

    Statement update(connection_.native_handle(),
                     "UPDATE project_metadata SET saved_revision = CASE "
                     "WHEN saved_revision < ? THEN ? ELSE saved_revision END, "
                     "heartbeat_utc_ms = ? WHERE singleton = 1");
    update.bind_int64(1, to_sql_revision(expected_revision));
    update.bind_int64(2, to_sql_revision(expected_revision));
    update.bind_int64(3, now_utc_ms());
    if (update.step() != SQLITE_DONE || sqlite3_changes(connection_.native_handle()) != 1) {
      throw ProjectStoreError("Could not record the completed checkpoint revision");
    }
    update_source.commit();
  }

  return expected_revision;
}

} // namespace video_editor::store

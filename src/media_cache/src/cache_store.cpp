// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/cache_store.h"

#include <openssl/evp.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace video_editor::media_cache {
namespace {

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

CacheError make_error(CacheErrorCode code, std::string message, int native_code = 0) {
  return CacheError{.code = code, .native_code = native_code, .message = std::move(message)};
}

[[nodiscard]] int current_native_code() noexcept {
#if defined(_WIN32)
  return 0;
#else
  return errno;
#endif
}

[[nodiscard]] std::int64_t now_utc_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// SQLite statement RAII wrapper. Mirrors the pattern in project_store.cpp but
// is local to this translation unit because the project_store Statement class
// is not exposed publicly.
// ---------------------------------------------------------------------------

class Statement final {
public:
  Statement(sqlite3* db, std::string_view sql) : db_(db) {
    const int result = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()),
                                          SQLITE_PREPARE_PERSISTENT, &stmt_, nullptr);
    if (result != SQLITE_OK) {
      last_error_message_ = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }

  ~Statement() noexcept {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  [[nodiscard]] bool valid() const noexcept { return stmt_ != nullptr; }
  [[nodiscard]] sqlite3_stmt* handle() const noexcept { return stmt_; }
  [[nodiscard]] std::string last_error() const noexcept { return last_error_message_; }

  void bind_text(int index, std::string_view value) {
    sqlite3_bind_text64(stmt_, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                        SQLITE_TRANSIENT, SQLITE_UTF8);
  }

  void bind_int(int index, int value) noexcept {
    sqlite3_bind_int(stmt_, index, value);
  }

  void bind_int64(int index, std::int64_t value) noexcept {
    sqlite3_bind_int64(stmt_, index, value);
  }

  int step() noexcept { return sqlite3_step(stmt_); }

  void reset() noexcept {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

  [[nodiscard]] std::string column_text(int index) const noexcept {
    const auto* value = sqlite3_column_text(stmt_, index);
    const int length = sqlite3_column_bytes(stmt_, index);
    if (value == nullptr) {
      return {};
    }
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(length)};
  }

  [[nodiscard]] std::int64_t column_int64(int index) const noexcept {
    return sqlite3_column_int64(stmt_, index);
  }

private:
  sqlite3* db_;
  sqlite3_stmt* stmt_{nullptr};
  std::string last_error_message_;
};

// ---------------------------------------------------------------------------
// SHA-256 of the composite key, returned as lowercase hex.
// ---------------------------------------------------------------------------

struct DigestContextDeleter {
  void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

[[nodiscard]] std::string key_blob_name(const CacheKey& key) {
  const std::string kind_str = std::to_string(static_cast<unsigned>(key.kind));
  const std::string material =
      key.asset_id + "\n" + kind_str + "\n" + key.parameter_hash;

  std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
  if (context == nullptr ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), material.data(), material.size()) != 1) {
    return {};
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) {
    return {};
  }

  std::ostringstream hex;
  hex << std::hex << std::setfill('0');
  for (unsigned index = 0; index < length; ++index) {
    hex << std::setw(2) << static_cast<unsigned>(digest[index]);
  }
  return hex.str();
}

[[nodiscard]] std::string composite_key(const CacheKey& key) {
  return key.asset_id + "\n" + std::to_string(static_cast<unsigned>(key.kind)) + "\n" +
         key.parameter_hash;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::filesystem::path blob_path_for(const std::filesystem::path& root,
                                                   const CacheKey& key) {
  return root / "blobs" / key_blob_name(key);
}

[[nodiscard]] bool write_blob_atomic(const std::filesystem::path& final_path,
                                      std::span<const std::byte> bytes,
                                      std::string& error_message) {
  namespace fs = std::filesystem;
  std::error_code ec;

  const fs::path parent = final_path.parent_path();
  if (!fs::exists(parent, ec)) {
    fs::create_directories(parent, ec);
    if (ec) {
      error_message = "cannot create blob directory: " + ec.message();
      return false;
    }
  }

  // Unique temp name in the same directory so the rename is atomic on the same
  // filesystem.
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  static std::atomic<std::uint64_t> sequence{0};
  fs::path temp_path =
      parent / (".blob-" + std::to_string(stamp) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp");

  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      error_message = "cannot open temp blob for writing";
      fs::remove(temp_path, ec);
      return false;
    }
    if (!bytes.empty()) {
      output.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      if (!output) {
        error_message = "cannot write blob bytes";
        output.close();
        fs::remove(temp_path, ec);
        return false;
      }
    }
    output.flush();
    output.close();
    if (output.fail()) {
      error_message = "cannot finalize blob write";
      fs::remove(temp_path, ec);
      return false;
    }
  }

#if !defined(_WIN32)
  // fsync the temp file before rename so the renamed file is durable on POSIX.
  {
    const int fd = ::open(temp_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      int result = 0;
      do {
        result = ::fsync(fd);
      } while (result != 0 && errno == EINTR);
      const int saved_errno = errno;
      ::close(fd);
      if (result != 0) {
        error_message = std::string("fsync of blob failed: ") + std::strerror(saved_errno);
        fs::remove(temp_path, ec);
        return false;
      }
    }
  }
#endif

  fs::rename(temp_path, final_path, ec);
  if (ec) {
    error_message = "cannot rename temp blob into place: " + ec.message();
    fs::remove(temp_path, ec);
    return false;
  }

#if !defined(_WIN32)
  // fsync the directory so the rename survives a crash.
  {
    const int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
      int result = 0;
      do {
        result = ::fsync(dir_fd);
      } while (result != 0 && errno == EINTR);
      ::close(dir_fd);
      if (result != 0) {
        // Non-fatal: the file is already in place; durability only.
      }
    }
  }
#endif

  return true;
}

[[nodiscard]] std::filesystem::path unique_temp_sibling(const std::filesystem::path& final_path) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  static std::atomic<std::uint64_t> sequence{0};
  return final_path.parent_path() /
         (".blob-" + std::to_string(stamp) + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp");
}

[[nodiscard]] bool fsync_regular_file(const std::filesystem::path& path, std::string& error_message) {
#if !defined(_WIN32)
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    int result = 0;
    do {
      result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    const int saved_errno = errno;
    ::close(fd);
    if (result != 0) {
      error_message = std::string("fsync of blob failed: ") + std::strerror(saved_errno);
      return false;
    }
  }
#else
  static_cast<void>(path);
  static_cast<void>(error_message);
#endif
  return true;
}

void fsync_directory_best_effort(const std::filesystem::path& directory) {
#if !defined(_WIN32)
  const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd >= 0) {
    int result = 0;
    do {
      result = ::fsync(dir_fd);
    } while (result != 0 && errno == EINTR);
    ::close(dir_fd);
  }
#else
  static_cast<void>(directory);
#endif
}

[[nodiscard]] bool adopt_blob_atomic(const std::filesystem::path& source,
                                     const std::filesystem::path& final_path,
                                     std::string& error_message) {
  namespace fs = std::filesystem;
  std::error_code ec;

  const fs::path parent = final_path.parent_path();
  if (!fs::exists(parent, ec)) {
    fs::create_directories(parent, ec);
    if (ec) {
      error_message = "cannot create blob directory: " + ec.message();
      return false;
    }
  }

  ec.clear();
  if (fs::exists(final_path, ec) && !ec) {
    ec.clear();
    if (fs::equivalent(source, final_path, ec) && !ec) {
      return true;
    }
  }
  ec.clear();

  const fs::path temp_path = unique_temp_sibling(final_path);
  fs::create_hard_link(source, temp_path, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(source, temp_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error_message = "cannot copy source into cache: " + ec.message();
      fs::remove(temp_path, ec);
      return false;
    }
  }

  if (!fsync_regular_file(temp_path, error_message)) {
    fs::remove(temp_path, ec);
    return false;
  }

  fs::rename(temp_path, final_path, ec);
  if (ec) {
    error_message = "cannot rename temp blob into place: " + ec.message();
    fs::remove(temp_path, ec);
    return false;
  }

  fsync_directory_best_effort(parent);
  return true;
}

[[nodiscard]] bool read_blob(const std::filesystem::path& path, std::vector<std::byte>& out,
                             std::string& error_message) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error_message = "cannot open blob for reading";
    return false;
  }
  input.seekg(0, std::ios::end);
  const auto end_pos = input.tellg();
  if (end_pos == std::streampos(-1)) {
    error_message = "cannot determine blob size";
    return false;
  }
  input.seekg(0, std::ios::beg);

  const auto size = static_cast<std::size_t>(end_pos);
  out.resize(size);
  if (size > 0) {
    input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!input) {
      error_message = "cannot read blob bytes";
      return false;
    }
  }
  return true;
}

void remove_blob_quietly(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

} // namespace

// ---------------------------------------------------------------------------
// PIMPL implementation
// ---------------------------------------------------------------------------

struct CacheStore::Impl {
  sqlite3* db{nullptr};
  std::filesystem::path root;

  // Prepared statements, lazily created on first use.
  std::unique_ptr<Statement> stmt_insert;
  std::unique_ptr<Statement> stmt_select_row;
  std::unique_ptr<Statement> stmt_select_blob_path;
  std::unique_ptr<Statement> stmt_contains;
  std::unique_ptr<Statement> stmt_delete_one;
  std::unique_ptr<Statement> stmt_delete_asset;
  std::unique_ptr<Statement> stmt_delete_kind;
  std::unique_ptr<Statement> stmt_touch;
  std::unique_ptr<Statement> stmt_total_bytes;
  std::unique_ptr<Statement> stmt_evict_select;
  std::unique_ptr<Statement> stmt_evict_delete;
  std::unique_ptr<Statement> stmt_inspect_select;
  std::unique_ptr<Statement> stmt_clear;

  // Keys that must not be evicted during the current put() call. Populated by
  // put() before invoking evict_to_budget() and cleared afterwards.
  std::unordered_set<std::string> protected_keys;

  // Monotonic timestamp guard. system_clock has millisecond resolution, so a
  // burst of operations can land in the same millisecond and produce identical
  // last_access_utc_ms values, which makes LRU eviction order nondeterministic.
  // We guarantee strictly increasing timestamps by bumping the last-assigned
  // value by 1 ms whenever the wall clock has not advanced. The skew is at most
  // a few milliseconds per burst and is harmless for eviction ordering.
  std::int64_t last_assigned_ms{0};

  explicit Impl(std::filesystem::path r) : root(std::move(r)) {}

  // Returns a timestamp that is strictly greater than the last value this
  // method returned, while staying as close as possible to wall-clock now.
  [[nodiscard]] std::int64_t monotonic_now_ms() noexcept {
    std::int64_t now = now_utc_ms();
    if (now <= last_assigned_ms) {
      now = last_assigned_ms + 1;
    }
    last_assigned_ms = now;
    return now;
  }

  ~Impl() {
    if (db != nullptr) {
      // Statements must be finalized before the connection closes.
      stmt_insert.reset();
      stmt_select_row.reset();
      stmt_select_blob_path.reset();
      stmt_contains.reset();
      stmt_delete_one.reset();
      stmt_delete_asset.reset();
      stmt_delete_kind.reset();
      stmt_touch.reset();
      stmt_total_bytes.reset();
      stmt_evict_select.reset();
      stmt_evict_delete.reset();
      stmt_inspect_select.reset();
      stmt_clear.reset();
      sqlite3_close_v2(db);
      db = nullptr;
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  [[nodiscard]] CacheResult<void> open_or_create(const CacheStoreOptions& options) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::create_directories(root, ec);
    if (ec) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::OpenFailed, "cannot create cache root: " + ec.message()));
    }
    fs::create_directories(root / "blobs", ec);
    if (ec) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::OpenFailed, "cannot create blobs directory: " + ec.message()));
    }

    const fs::path db_path = root / "index.sqlite";
    const std::string encoded = db_path.string();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
                      SQLITE_OPEN_EXRESCODE;
    int result = sqlite3_open_v2(encoded.c_str(), &db, flags, nullptr);
    if (result != SQLITE_OK) {
      const std::string msg = db != nullptr ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
      sqlite3_close_v2(db);
      db = nullptr;
      return CacheResult<void>::failure(make_error(CacheErrorCode::OpenFailed, msg));
    }

    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 5000);

    // FULL journal + synchronous=FULL for durability. WAL is unnecessary here
    // because the store is single-writer and callers serialize access.
    if (sqlite3_exec(db, "PRAGMA journal_mode = TRUNCATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::OpenFailed, sqlite3_errmsg(db)));
    }
    if (sqlite3_exec(db, "PRAGMA synchronous = FULL;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::OpenFailed, sqlite3_errmsg(db)));
    }

    constexpr std::string_view kSchemaSql =
        "CREATE TABLE IF NOT EXISTS cache_entries ("
        "asset_id TEXT NOT NULL,"
        "kind INTEGER NOT NULL,"
        "parameter_hash TEXT NOT NULL,"
        "blob_path TEXT NOT NULL,"
        "bytes INTEGER NOT NULL,"
        "last_access_utc_ms INTEGER NOT NULL,"
        "created_utc_ms INTEGER NOT NULL,"
        "PRIMARY KEY (asset_id, kind, parameter_hash)"
        ");";
    if (sqlite3_exec(db, kSchemaSql.data(), nullptr, nullptr, nullptr) != SQLITE_OK) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::OpenFailed, sqlite3_errmsg(db)));
    }

    if (options.integrity_check) {
      sqlite3_stmt* check_stmt = nullptr;
      if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &check_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(check_stmt) == SQLITE_ROW) {
          const auto* value = sqlite3_column_text(check_stmt, 0);
          if (value == nullptr || std::strcmp(reinterpret_cast<const char*>(value), "ok") != 0) {
            sqlite3_finalize(check_stmt);
            return CacheResult<void>::failure(make_error(
                CacheErrorCode::Internal, "cache index failed integrity check"));
          }
        }
        sqlite3_finalize(check_stmt);
      }
    }

    return CacheResult<void>::success();
  }

  [[nodiscard]] Statement* ensure_insert() {
    if (stmt_insert == nullptr) {
      stmt_insert = std::make_unique<Statement>(
          db,
          "INSERT OR REPLACE INTO cache_entries (asset_id, kind, parameter_hash, blob_path, "
          "bytes, last_access_utc_ms, created_utc_ms) VALUES (?, ?, ?, ?, ?, ?, ?);");
    }
    return stmt_insert.get();
  }

  [[nodiscard]] Statement* ensure_select_row() {
    if (stmt_select_row == nullptr) {
      stmt_select_row = std::make_unique<Statement>(
          db, "SELECT bytes, blob_path, last_access_utc_ms FROM cache_entries "
              "WHERE asset_id = ? AND kind = ? AND parameter_hash = ?;");
    }
    return stmt_select_row.get();
  }

  [[nodiscard]] Statement* ensure_contains() {
    if (stmt_contains == nullptr) {
      stmt_contains = std::make_unique<Statement>(
          db, "SELECT 1 FROM cache_entries WHERE asset_id = ? AND kind = ? AND parameter_hash = ?;");
    }
    return stmt_contains.get();
  }

  [[nodiscard]] Statement* ensure_delete_one() {
    if (stmt_delete_one == nullptr) {
      stmt_delete_one = std::make_unique<Statement>(
          db, "DELETE FROM cache_entries WHERE asset_id = ? AND kind = ? AND parameter_hash = ?;");
    }
    return stmt_delete_one.get();
  }

  [[nodiscard]] Statement* ensure_delete_asset() {
    if (stmt_delete_asset == nullptr) {
      stmt_delete_asset = std::make_unique<Statement>(
          db, "SELECT blob_path FROM cache_entries WHERE asset_id = ?;");
    }
    return stmt_delete_asset.get();
  }

  [[nodiscard]] Statement* ensure_delete_kind() {
    if (stmt_delete_kind == nullptr) {
      stmt_delete_kind = std::make_unique<Statement>(
          db, "SELECT blob_path FROM cache_entries WHERE asset_id = ? AND kind = ?;");
    }
    return stmt_delete_kind.get();
  }

  [[nodiscard]] Statement* ensure_touch() {
    if (stmt_touch == nullptr) {
      stmt_touch = std::make_unique<Statement>(
          db, "UPDATE cache_entries SET last_access_utc_ms = ? "
              "WHERE asset_id = ? AND kind = ? AND parameter_hash = ?;");
    }
    return stmt_touch.get();
  }

  [[nodiscard]] Statement* ensure_total_bytes() {
    if (stmt_total_bytes == nullptr) {
      stmt_total_bytes = std::make_unique<Statement>(db, "SELECT COALESCE(SUM(bytes), 0) FROM cache_entries;");
    }
    return stmt_total_bytes.get();
  }

  [[nodiscard]] Statement* ensure_evict_select() {
    if (stmt_evict_select == nullptr) {
      stmt_evict_select = std::make_unique<Statement>(
          db, "SELECT asset_id, kind, parameter_hash, blob_path, bytes "
              "FROM cache_entries ORDER BY last_access_utc_ms ASC;");
    }
    return stmt_evict_select.get();
  }

  [[nodiscard]] Statement* ensure_evict_delete() {
    if (stmt_evict_delete == nullptr) {
      stmt_evict_delete = std::make_unique<Statement>(
          db, "DELETE FROM cache_entries WHERE asset_id = ? AND kind = ? AND parameter_hash = ?;");
    }
    return stmt_evict_delete.get();
  }

  [[nodiscard]] Statement* ensure_inspect_select() {
    if (stmt_inspect_select == nullptr) {
      stmt_inspect_select = std::make_unique<Statement>(
          db, "SELECT asset_id, kind, parameter_hash, bytes, last_access_utc_ms "
              "FROM cache_entries ORDER BY last_access_utc_ms ASC;");
    }
    return stmt_inspect_select.get();
  }

  [[nodiscard]] Statement* ensure_clear() {
    if (stmt_clear == nullptr) {
      stmt_clear = std::make_unique<Statement>(db, "DELETE FROM cache_entries;");
    }
    return stmt_clear.get();
  }

  [[nodiscard]] std::uint64_t total_bytes() noexcept {
    Statement* stmt = ensure_total_bytes();
    if (stmt == nullptr || !stmt->valid()) {
      return 0;
    }
    if (stmt->step() == SQLITE_ROW) {
      const std::int64_t value = stmt->column_int64(0);
      stmt->reset();
      return value < 0 ? 0U : static_cast<std::uint64_t>(value);
    }
    stmt->reset();
    return 0;
  }

  // Evicts LRU entries until total bytes are at or below budget. Entries whose
  // composite key is in protected_keys are skipped. Returns the count evicted.
  [[nodiscard]] std::uint64_t evict_to_budget(std::uint64_t budget) {
    if (budget == 0) {
      return 0;
    }

    Statement* select = ensure_evict_select();
    if (select == nullptr || !select->valid()) {
      return 0;
    }

    std::uint64_t total = total_bytes();
    if (total <= budget) {
      select->reset();
      return 0;
    }

    std::uint64_t evicted = 0;
    // Collect candidate rows first so we are not interleaving SELECT and DELETE
    // on the same statement handle.
    struct Row {
      std::string asset_id;
      int kind;
      std::string parameter_hash;
      std::string blob_path;
      std::uint64_t bytes;
    };
    std::vector<Row> rows;
    while (select->step() == SQLITE_ROW) {
      Row row;
      row.asset_id = select->column_text(0);
      row.kind = static_cast<int>(sqlite3_column_int(select->handle(), 1));
      row.parameter_hash = select->column_text(2);
      row.blob_path = select->column_text(3);
      const std::int64_t b = select->column_int64(4);
      row.bytes = b < 0 ? 0U : static_cast<std::uint64_t>(b);
      rows.push_back(std::move(row));
    }
    select->reset();

    Statement* del = ensure_evict_delete();
    if (del == nullptr || !del->valid()) {
      return 0;
    }

    for (const auto& row : rows) {
      if (total <= budget) {
        break;
      }
      const std::string key = row.asset_id + "\n" + std::to_string(row.kind) + "\n" +
                              row.parameter_hash;
      if (protected_keys.find(key) != protected_keys.end()) {
        continue;
      }

      del->bind_text(1, row.asset_id);
      del->bind_int(2, row.kind);
      del->bind_text(3, row.parameter_hash);
      if (del->step() == SQLITE_DONE) {
        if (row.bytes <= total) {
          total -= row.bytes;
        } else {
          total = 0;
        }
        ++evicted;
        remove_blob_quietly(std::filesystem::path(row.blob_path));
      }
      del->reset();
    }

    return evicted;
  }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

CacheStore::CacheStore(std::filesystem::path root, CacheStoreOptions options)
    : root_(std::move(root)), options_(options), impl_(std::make_unique<Impl>(root_)) {
  auto result = impl_->open_or_create(options_);
  if (!result.has_value()) {
    const CacheError error = result.error();
    throw std::runtime_error("CacheStore open failed: " + error.message);
  }
}

CacheStore::~CacheStore() = default;

CacheStore::CacheStore(CacheStore&&) noexcept = default;
CacheStore& CacheStore::operator=(CacheStore&&) noexcept = default;

CacheResult<void> CacheStore::put(const CacheKey& key, std::span<const std::byte> bytes) {
  if (!key.valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }

  const std::uint64_t blob_bytes = static_cast<std::uint64_t>(bytes.size());
  if (options_.budget_bytes != 0 && blob_bytes > options_.budget_bytes) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Full,
                   "blob size " + std::to_string(blob_bytes) + " exceeds budget " +
                       std::to_string(options_.budget_bytes)));
  }

  const std::filesystem::path blob_path = blob_path_for(root_, key);
  if (blob_path.empty()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot compute blob name"));
  }

  std::string write_error;
  if (!write_blob_atomic(blob_path, bytes, write_error)) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::WriteFailed, std::move(write_error), current_native_code()));
  }

  const std::int64_t now = impl_->monotonic_now_ms();
  Statement* insert = impl_->ensure_insert();
  if (insert == nullptr || !insert->valid()) {
    remove_blob_quietly(blob_path);
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare insert statement"));
  }

  insert->bind_text(1, key.asset_id);
  insert->bind_int(2, static_cast<int>(key.kind));
  insert->bind_text(3, key.parameter_hash);
  insert->bind_text(4, blob_path.string());
  insert->bind_int64(5, static_cast<std::int64_t>(blob_bytes));
  insert->bind_int64(6, now);
  insert->bind_int64(7, now);
  const int step_result = insert->step();
  insert->reset();

  if (step_result != SQLITE_DONE) {
    remove_blob_quietly(blob_path);
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot insert cache entry"));
  }

  // Protect the just-inserted entry from eviction, then evict to budget.
  impl_->protected_keys.insert(composite_key(key));
  (void)impl_->evict_to_budget(options_.budget_bytes);
  impl_->protected_keys.clear();

  return CacheResult<void>::success();
}

CacheResult<void> CacheStore::put_file(const CacheKey& key, const std::filesystem::path& source) {
  if (!key.valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }
  if (source.empty()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::InvalidArgument, "source path is empty"));
  }

  std::error_code ec;
  if (!std::filesystem::exists(source, ec) || ec) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::NotFound, "source file does not exist", current_native_code()));
  }

  ec.clear();
  const auto size = std::filesystem::file_size(source, ec);
  if (ec) {
    return CacheResult<void>::failure(make_error(CacheErrorCode::WriteFailed,
                                                 "cannot determine source size: " + ec.message(),
                                                 current_native_code()));
  }
  const std::uint64_t blob_bytes = static_cast<std::uint64_t>(size);
  if (options_.budget_bytes != 0 && blob_bytes > options_.budget_bytes) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Full,
                   "blob size " + std::to_string(blob_bytes) + " exceeds budget " +
                       std::to_string(options_.budget_bytes)));
  }

  const std::filesystem::path blob_path = blob_path_for(root_, key);
  if (blob_path.empty()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot compute blob name"));
  }

  bool source_is_destination = false;
  {
    std::error_code equiv_ec;
    if (std::filesystem::exists(blob_path, equiv_ec) && !equiv_ec) {
      equiv_ec.clear();
      source_is_destination =
          std::filesystem::equivalent(source, blob_path, equiv_ec) && !equiv_ec;
    }
  }

  if (!source_is_destination) {
    std::string write_error;
    if (!adopt_blob_atomic(source, blob_path, write_error)) {
      return CacheResult<void>::failure(
          make_error(CacheErrorCode::WriteFailed, std::move(write_error), current_native_code()));
    }
  }

  const std::int64_t now = impl_->monotonic_now_ms();
  Statement* insert = impl_->ensure_insert();
  if (insert == nullptr || !insert->valid()) {
    if (!source_is_destination) {
      remove_blob_quietly(blob_path);
    }
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare insert statement"));
  }

  insert->bind_text(1, key.asset_id);
  insert->bind_int(2, static_cast<int>(key.kind));
  insert->bind_text(3, key.parameter_hash);
  insert->bind_text(4, blob_path.string());
  insert->bind_int64(5, static_cast<std::int64_t>(blob_bytes));
  insert->bind_int64(6, now);
  insert->bind_int64(7, now);
  const int step_result = insert->step();
  insert->reset();

  if (step_result != SQLITE_DONE) {
    if (!source_is_destination) {
      remove_blob_quietly(blob_path);
    }
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot insert cache entry"));
  }

  if (!source_is_destination) {
    remove_blob_quietly(source);
  }

  impl_->protected_keys.insert(composite_key(key));
  (void)impl_->evict_to_budget(options_.budget_bytes);
  impl_->protected_keys.clear();

  return CacheResult<void>::success();
}

CacheResult<std::vector<std::byte>> CacheStore::get(const CacheKey& key) {
  if (!key.valid()) {
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }

  Statement* select = impl_->ensure_select_row();
  if (select == nullptr || !select->valid()) {
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare select statement"));
  }

  select->bind_text(1, key.asset_id);
  select->bind_int(2, static_cast<int>(key.kind));
  select->bind_text(3, key.parameter_hash);
  const int step_result = select->step();
  if (step_result != SQLITE_ROW) {
    select->reset();
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::NotFound, "cache entry not found"));
  }

  const std::string blob_path_str = select->column_text(1);
  select->reset();

  if (blob_path_str.empty()) {
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::NotFound, "cache entry has no blob path"));
  }

  const std::filesystem::path blob_path(blob_path_str);
  std::error_code ec;
  if (!std::filesystem::exists(blob_path, ec)) {
    // Treat a missing blob file as NotFound, not Internal.
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::NotFound, "blob file is missing"));
  }

  std::vector<std::byte> out;
  std::string read_error;
  if (!read_blob(blob_path, out, read_error)) {
    return CacheResult<std::vector<std::byte>>::failure(
        make_error(CacheErrorCode::ReadFailed, std::move(read_error), current_native_code()));
  }

  // Touch the access time.
  Statement* touch = impl_->ensure_touch();
  if (touch != nullptr && touch->valid()) {
    touch->bind_int64(1, impl_->monotonic_now_ms());
    touch->bind_text(2, key.asset_id);
    touch->bind_int(3, static_cast<int>(key.kind));
    touch->bind_text(4, key.parameter_hash);
    touch->step();
    touch->reset();
  }

  return CacheResult<std::vector<std::byte>>::success(std::move(out));
}

CacheResult<std::filesystem::path> CacheStore::path_for(const CacheKey& key) {
  if (!key.valid()) {
    return CacheResult<std::filesystem::path>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }

  Statement* select = impl_->ensure_select_row();
  if (select == nullptr || !select->valid()) {
    return CacheResult<std::filesystem::path>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare select statement"));
  }

  select->bind_text(1, key.asset_id);
  select->bind_int(2, static_cast<int>(key.kind));
  select->bind_text(3, key.parameter_hash);
  const int step_result = select->step();
  if (step_result != SQLITE_ROW) {
    select->reset();
    return CacheResult<std::filesystem::path>::failure(
        make_error(CacheErrorCode::NotFound, "cache entry not found"));
  }

  const std::string blob_path_str = select->column_text(1);
  select->reset();

  if (blob_path_str.empty()) {
    return CacheResult<std::filesystem::path>::failure(
        make_error(CacheErrorCode::NotFound, "cache entry has no blob path"));
  }

  const std::filesystem::path blob_path(blob_path_str);
  std::error_code ec;
  if (!std::filesystem::exists(blob_path, ec)) {
    return CacheResult<std::filesystem::path>::failure(
        make_error(CacheErrorCode::NotFound, "blob file is missing"));
  }

  Statement* touch = impl_->ensure_touch();
  if (touch != nullptr && touch->valid()) {
    touch->bind_int64(1, impl_->monotonic_now_ms());
    touch->bind_text(2, key.asset_id);
    touch->bind_int(3, static_cast<int>(key.kind));
    touch->bind_text(4, key.parameter_hash);
    touch->step();
    touch->reset();
  }

  return CacheResult<std::filesystem::path>::success(blob_path);
}

CacheResult<bool> CacheStore::contains(const CacheKey& key) {
  if (!key.valid()) {
    return CacheResult<bool>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }

  Statement* stmt = impl_->ensure_contains();
  if (stmt == nullptr || !stmt->valid()) {
    return CacheResult<bool>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare contains statement"));
  }

  stmt->bind_text(1, key.asset_id);
  stmt->bind_int(2, static_cast<int>(key.kind));
  stmt->bind_text(3, key.parameter_hash);
  const int step_result = stmt->step();
  const bool found = (step_result == SQLITE_ROW);
  stmt->reset();

  if (step_result != SQLITE_ROW && step_result != SQLITE_DONE) {
    return CacheResult<bool>::failure(
        make_error(CacheErrorCode::Internal, "cannot query cache entry"));
  }

  return CacheResult<bool>::success(found);
}

CacheResult<void> CacheStore::remove(const CacheKey& key) {
  if (!key.valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::InvalidArgument, "cache key is empty"));
  }

  // Check existence first so we can return NotFound for missing rows.
  Statement* contains = impl_->ensure_contains();
  if (contains == nullptr || !contains->valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare contains statement"));
  }
  contains->bind_text(1, key.asset_id);
  contains->bind_int(2, static_cast<int>(key.kind));
  contains->bind_text(3, key.parameter_hash);
  const int check_result = contains->step();
  contains->reset();
  if (check_result != SQLITE_ROW) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::NotFound, "cache entry not found"));
  }

  Statement* del = impl_->ensure_delete_one();
  if (del == nullptr || !del->valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare delete statement"));
  }
  del->bind_text(1, key.asset_id);
  del->bind_int(2, static_cast<int>(key.kind));
  del->bind_text(3, key.parameter_hash);
  del->step();
  del->reset();

  remove_blob_quietly(blob_path_for(root_, key));
  return CacheResult<void>::success();
}

CacheResult<std::uint64_t> CacheStore::remove_asset(const std::string& asset_id) {
  if (asset_id.empty()) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::InvalidArgument, "asset id is empty"));
  }

  Statement* select = impl_->ensure_delete_asset();
  if (select == nullptr || !select->valid()) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare select statement"));
  }

  select->bind_text(1, asset_id);
  std::vector<std::string> blob_paths;
  while (select->step() == SQLITE_ROW) {
    blob_paths.push_back(select->column_text(0));
  }
  select->reset();

  if (blob_paths.empty()) {
    return CacheResult<std::uint64_t>::success(0);
  }

  // Delete all rows for this asset via a direct exec to avoid statement
  // reuse complexity.
  sqlite3_stmt* del_stmt = nullptr;
  if (sqlite3_prepare_v2(impl_->db, "DELETE FROM cache_entries WHERE asset_id = ?;", -1,
                         &del_stmt, nullptr) != SQLITE_OK) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare delete-asset statement"));
  }
  sqlite3_bind_text64(del_stmt, 1, asset_id.data(),
                      static_cast<sqlite3_uint64>(asset_id.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
  sqlite3_step(del_stmt);
  sqlite3_finalize(del_stmt);

  for (const auto& path_str : blob_paths) {
    remove_blob_quietly(std::filesystem::path(path_str));
  }

  return CacheResult<std::uint64_t>::success(static_cast<std::uint64_t>(blob_paths.size()));
}

CacheResult<std::uint64_t> CacheStore::remove_kind(const std::string& asset_id, CacheKind kind) {
  if (asset_id.empty()) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::InvalidArgument, "asset id is empty"));
  }

  Statement* select = impl_->ensure_delete_kind();
  if (select == nullptr || !select->valid()) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare select statement"));
  }

  select->bind_text(1, asset_id);
  select->bind_int(2, static_cast<int>(kind));
  std::vector<std::string> blob_paths;
  while (select->step() == SQLITE_ROW) {
    blob_paths.push_back(select->column_text(0));
  }
  select->reset();

  if (blob_paths.empty()) {
    return CacheResult<std::uint64_t>::success(0);
  }

  sqlite3_stmt* del_stmt = nullptr;
  if (sqlite3_prepare_v2(impl_->db,
                         "DELETE FROM cache_entries WHERE asset_id = ? AND kind = ?;", -1,
                         &del_stmt, nullptr) != SQLITE_OK) {
    return CacheResult<std::uint64_t>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare delete-kind statement"));
  }
  sqlite3_bind_text64(del_stmt, 1, asset_id.data(),
                      static_cast<sqlite3_uint64>(asset_id.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
  sqlite3_bind_int(del_stmt, 2, static_cast<int>(kind));
  sqlite3_step(del_stmt);
  sqlite3_finalize(del_stmt);

  for (const auto& path_str : blob_paths) {
    remove_blob_quietly(std::filesystem::path(path_str));
  }

  return CacheResult<std::uint64_t>::success(static_cast<std::uint64_t>(blob_paths.size()));
}

CacheResult<std::uint64_t> CacheStore::evict_to_budget() {
  return CacheResult<std::uint64_t>::success(impl_->evict_to_budget(options_.budget_bytes));
}

CacheResult<void> CacheStore::clear() {
  Statement* stmt = impl_->ensure_clear();
  if (stmt == nullptr || !stmt->valid()) {
    return CacheResult<void>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare clear statement"));
  }
  stmt->step();
  stmt->reset();

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path blobs_dir = root_ / "blobs";
  if (fs::exists(blobs_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(blobs_dir, ec)) {
      if (ec) {
        break;
      }
      std::error_code remove_ec;
      fs::remove(entry.path(), remove_ec);
    }
  }

  return CacheResult<void>::success();
}

CacheResult<CacheInventory> CacheStore::inspect() {
  Statement* stmt = impl_->ensure_inspect_select();
  if (stmt == nullptr || !stmt->valid()) {
    return CacheResult<CacheInventory>::failure(
        make_error(CacheErrorCode::Internal, "cannot prepare inspect statement"));
  }

  CacheInventory inventory;
  inventory.budget_bytes = options_.budget_bytes;

  while (stmt->step() == SQLITE_ROW) {
    CacheInventoryEntry entry;
    entry.key.asset_id = stmt->column_text(0);
    entry.key.kind = static_cast<CacheKind>(sqlite3_column_int(stmt->handle(), 1));
    entry.key.parameter_hash = stmt->column_text(2);
    const std::int64_t b = stmt->column_int64(3);
    entry.bytes = b < 0 ? 0U : static_cast<std::uint64_t>(b);
    entry.last_access_utc_ms = stmt->column_int64(4);
    inventory.total_bytes += entry.bytes;
    inventory.entries.push_back(std::move(entry));
  }
  stmt->reset();

  return CacheResult<CacheInventory>::success(std::move(inventory));
}

} // namespace video_editor::media_cache

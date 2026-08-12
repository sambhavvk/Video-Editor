// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;

namespace video_editor::store {

class SqliteError : public std::runtime_error {
public:
  SqliteError(std::string message, int code, int extended_code);

  [[nodiscard]] int code() const noexcept {
    return code_;
  }
  [[nodiscard]] int extended_code() const noexcept {
    return extended_code_;
  }

private:
  int code_;
  int extended_code_;
};

enum class SqliteOpenMode {
  kReadOnly,
  kReadWrite,
  kReadWriteCreate,
};

// A move-only owner for one SQLite connection. The native handle is exposed so
// narrowly-scoped module code can use prepared statements and sqlite3_backup.
class SqliteConnection final {
public:
  explicit SqliteConnection(std::filesystem::path path,
                            SqliteOpenMode mode = SqliteOpenMode::kReadWriteCreate,
                            int busy_timeout_ms = 5'000);
  ~SqliteConnection() noexcept;

  SqliteConnection(const SqliteConnection&) = delete;
  SqliteConnection& operator=(const SqliteConnection&) = delete;
  SqliteConnection(SqliteConnection&& other) noexcept;
  SqliteConnection& operator=(SqliteConnection&& other) noexcept;

  void execute(std::string_view sql);

  [[nodiscard]] sqlite3* native_handle() const noexcept {
    return db_;
  }
  [[nodiscard]] bool is_open() const noexcept {
    return db_ != nullptr;
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  void close() noexcept;

  sqlite3* db_ = nullptr;
  std::filesystem::path path_;
};

} // namespace video_editor::store

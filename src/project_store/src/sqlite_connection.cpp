// SPDX-License-Identifier: MPL-2.0

#include "video_editor/project_store/sqlite_connection.hpp"

#include <sqlite3.h>

#include <utility>

namespace video_editor::store {
namespace {

std::string path_to_utf8(const std::filesystem::path& path) {
  const auto encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::string sqlite_message(sqlite3* db, const std::string_view context) {
  std::string message(context);
  message.append(": ");
  message.append(db != nullptr ? sqlite3_errmsg(db) : "SQLite connection error");
  return message;
}

} // namespace

SqliteError::SqliteError(std::string message, const int code, const int extended_code)
    : std::runtime_error(std::move(message)), code_(code), extended_code_(extended_code) {}

SqliteConnection::SqliteConnection(std::filesystem::path path, const SqliteOpenMode mode,
                                   const int busy_timeout_ms)
    : path_(std::move(path)) {
  if (path_.empty()) {
    throw std::invalid_argument("SQLite database path must not be empty");
  }
  if (busy_timeout_ms < 0) {
    throw std::invalid_argument("SQLite busy timeout must not be negative");
  }

  int flags = SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_EXRESCODE;
  switch (mode) {
  case SqliteOpenMode::kReadOnly:
    flags |= SQLITE_OPEN_READONLY;
    break;
  case SqliteOpenMode::kReadWrite:
    flags |= SQLITE_OPEN_READWRITE;
    break;
  case SqliteOpenMode::kReadWriteCreate:
    flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    break;
  }

  const std::string encoded_path = path_to_utf8(path_);
  const int open_result = sqlite3_open_v2(encoded_path.c_str(), &db_, flags, nullptr);
  if (open_result != SQLITE_OK) {
    const int extended = db_ != nullptr ? sqlite3_extended_errcode(db_) : open_result;
    const std::string message = sqlite_message(db_, "Failed to open SQLite database");
    close();
    throw SqliteError(message, open_result, extended);
  }

  sqlite3_extended_result_codes(db_, 1);
  const int timeout_result = sqlite3_busy_timeout(db_, busy_timeout_ms);
  if (timeout_result != SQLITE_OK) {
    const int extended = sqlite3_extended_errcode(db_);
    const std::string message = sqlite_message(db_, "Failed to set SQLite busy timeout");
    close();
    throw SqliteError(message, timeout_result, extended);
  }
}

SqliteConnection::~SqliteConnection() noexcept {
  close();
}

SqliteConnection::SqliteConnection(SqliteConnection&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)) {}

SqliteConnection& SqliteConnection::operator=(SqliteConnection&& other) noexcept {
  if (this != &other) {
    close();
    db_ = std::exchange(other.db_, nullptr);
    path_ = std::move(other.path_);
  }
  return *this;
}

void SqliteConnection::execute(const std::string_view sql) {
  if (db_ == nullptr) {
    throw std::logic_error("Cannot execute SQL on a closed connection");
  }

  const std::string statement(sql);
  char* error_message = nullptr;
  const int result = sqlite3_exec(db_, statement.c_str(), nullptr, nullptr, &error_message);
  if (result == SQLITE_OK) {
    return;
  }

  std::string message = "SQLite statement failed: ";
  if (error_message != nullptr) {
    message.append(error_message);
    sqlite3_free(error_message);
  } else {
    message.append(sqlite3_errmsg(db_));
  }
  throw SqliteError(std::move(message), result, sqlite3_extended_errcode(db_));
}

void SqliteConnection::close() noexcept {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

} // namespace video_editor::store

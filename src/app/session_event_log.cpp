// SPDX-License-Identifier: MPL-2.0
#include "session_event_log.hpp"
#include "path_utils.hpp"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QStandardPaths>
#include <QWidget>

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <unistd.h>

#include <csignal>

namespace video_editor::app {
namespace {

constexpr const char* kLogsSubdir = "logs";
constexpr const char* kCurrentSessionMarker = "current.session";
constexpr const char* kLastCrashMarker = "last-crash.log";
constexpr const char* kCrashFooterPrefix = "\nCRASH signal=";
constexpr const char* kShutdownFooter = "\nSHUTDOWN clean\n";

std::atomic<int> g_log_fd{-1};
std::atomic<bool> g_handlers_installed{false};

char g_last_crash_marker_path[4096]{};
char g_session_path_for_crash[4096]{};
std::atomic<bool> g_crash_paths_ready{false};

class SessionUiEventFilter final : public QObject {
public:
  explicit SessionUiEventFilter(QObject* parent = nullptr) : QObject(parent) {}

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::MouseButtonPress) {
      return QObject::eventFilter(watched, event);
    }
    const auto* mouse_event = dynamic_cast<QMouseEvent*>(event);
    if (mouse_event == nullptr) {
      return QObject::eventFilter(watched, event);
    }

    const auto* widget = qobject_cast<const QWidget*>(watched);
    if (widget == nullptr) {
      return QObject::eventFilter(watched, event);
    }

    QString object_name = widget->objectName();
    QString ancestors;
    if (object_name.isEmpty()) {
      QStringList chain;
      for (const QWidget* ancestor = widget->parentWidget(); ancestor != nullptr && chain.size() < 6;
           ancestor = ancestor->parentWidget()) {
        const QString name = ancestor->objectName();
        if (!name.isEmpty()) {
          chain.prepend(name);
        }
      }
      ancestors = chain.join(QLatin1Char('/'));
    }

    QString button;
    switch (mouse_event->button()) {
    case Qt::LeftButton:
      button = QStringLiteral("left");
      break;
    case Qt::RightButton:
      button = QStringLiteral("right");
      break;
    case Qt::MiddleButton:
      button = QStringLiteral("middle");
      break;
    case Qt::BackButton:
      button = QStringLiteral("back");
      break;
    case Qt::ForwardButton:
      button = QStringLiteral("forward");
      break;
    default:
      button = QStringLiteral("other");
      break;
    }

    std::string detail;
    if (!object_name.isEmpty()) {
      detail = "widget=" + object_name.toStdString() + " class=" + widget->metaObject()->className() +
               " button=" + button.toStdString();
    } else if (!ancestors.isEmpty()) {
      detail = "class=" + std::string(widget->metaObject()->className()) +
               " ancestors=" + ancestors.toStdString() + " button=" + button.toStdString();
    } else {
      detail = "class=" + std::string(widget->metaObject()->className()) +
               " button=" + button.toStdString();
    }

    SessionEventLog::instance().log_ui("click", detail);
    return QObject::eventFilter(watched, event);
  }
};

SessionUiEventFilter* g_ui_filter = nullptr;

[[nodiscard]] std::filesystem::path logsDirectory() {
  const QString root =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return pathFromQString(root) / kLogsSubdir;
}

[[nodiscard]] std::filesystem::path markerPath(const char* name) {
  return logsDirectory() / name;
}

void writeCrashMarkerPath() {
  const int fd = ::open(g_last_crash_marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  const std::size_t path_length = std::strlen(g_session_path_for_crash);
  if (path_length > 0U) {
    (void)::write(fd, g_session_path_for_crash, path_length);
    (void)::write(fd, "\n", 1);
  }
  (void)::fsync(fd);
  (void)::close(fd);
}

void writeSignalNumber(int fd, int signum) {
  char digits[16];
  int length = 0;
  unsigned value = static_cast<unsigned>(signum);
  do {
    digits[length++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value > 0U && length < static_cast<int>(sizeof(digits)));
  for (int index = length - 1; index >= 0; --index) {
    (void)::write(fd, &digits[index], 1);
  }
}

void writeCrashFooter(int fd, int signum) {
  if (fd < 0) {
    return;
  }
  (void)::write(fd, kCrashFooterPrefix, std::strlen(kCrashFooterPrefix));
  writeSignalNumber(fd, signum);
  (void)::write(fd, "\n", 1);
  (void)::fsync(fd);
}

void crashSignalHandler(int signum) {
  const int fd = g_log_fd.load(std::memory_order_acquire);
  writeCrashFooter(fd, signum);
  if (g_crash_paths_ready.load(std::memory_order_acquire)) {
    writeCrashMarkerPath();
  }

  std::signal(signum, SIG_DFL);
  std::raise(signum);
}

void installOneHandler(int signum) {
  struct sigaction action {};
  action.sa_handler = crashSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = static_cast<int>(SA_RESETHAND);
  (void)sigaction(signum, &action, nullptr);
}

[[nodiscard]] std::optional<std::filesystem::path> readMarkerPath(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  std::string line;
  if (!std::getline(input, line) || line.empty()) {
    return std::nullopt;
  }
  return pathFromUtf8String(line);
}

void consumeMarkerFile(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

[[nodiscard]] std::string currentTimestamp() {
  return QDateTime::currentDateTimeUtc()
      .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"))
      .toStdString();
}

void writeLineLocked(int fd, std::string_view category, std::string_view action,
                     std::string_view detail) {
  std::string line = currentTimestamp();
  line += ' ';
  line += category;
  line += ' ';
  line += action;
  if (!detail.empty()) {
    line += ' ';
    line += detail;
  }
  line += '\n';
  if (fd >= 0) {
    (void)::write(fd, line.data(), line.size());
  }
}

} // namespace

SessionEventLog& SessionEventLog::instance() {
  static SessionEventLog instance;
  return instance;
}

void SessionEventLog::install_crash_handlers() {
  if (g_handlers_installed.exchange(true)) {
    return;
  }
  installOneHandler(SIGSEGV);
  installOneHandler(SIGABRT);
  installOneHandler(SIGFPE);
  installOneHandler(SIGILL);
#ifndef _WIN32
  installOneHandler(SIGBUS);
#endif
}

void SessionEventLog::start() {
  std::lock_guard lock(mutex_);
  if (started_) {
    return;
  }

  install_crash_handlers();

  const auto logs_dir = logsDirectory();
  std::error_code error;
  std::filesystem::create_directories(logs_dir, error);

  const QString stamp =
      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  session_path_ = logs_dir / ("session-" + stamp.toStdString() + ".log");

  const int fd = ::open(session_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  log_fd_ = fd;
  g_log_fd.store(fd, std::memory_order_release);

  const std::string marker_utf8 = utf8StringFromPath(session_path_);
  {
    const auto current_marker = markerPath(kCurrentSessionMarker);
    std::ofstream marker(current_marker);
    if (marker) {
      marker << marker_utf8 << '\n';
    }
  }

  const auto last_crash_marker = markerPath(kLastCrashMarker);
  const std::string last_crash_utf8 = utf8StringFromPath(last_crash_marker);
  const std::string session_utf8 = utf8StringFromPath(session_path_);
  if (last_crash_utf8.size() + 1U < sizeof(g_last_crash_marker_path) &&
      session_utf8.size() + 1U < sizeof(g_session_path_for_crash)) {
    std::memcpy(g_last_crash_marker_path, last_crash_utf8.data(), last_crash_utf8.size());
    g_last_crash_marker_path[last_crash_utf8.size()] = '\0';
    std::memcpy(g_session_path_for_crash, session_utf8.data(), session_utf8.size());
    g_session_path_for_crash[session_utf8.size()] = '\0';
    g_crash_paths_ready.store(true, std::memory_order_release);
  }

  writeLineLocked(log_fd_, "SESSION", "start", marker_utf8);
  started_ = true;

  if (QApplication::instance() != nullptr && g_ui_filter == nullptr) {
    g_ui_filter = new SessionUiEventFilter(QApplication::instance());
    QApplication::instance()->installEventFilter(g_ui_filter);
  }
}

void SessionEventLog::shutdown() {
  std::lock_guard lock(mutex_);
  if (!started_ || log_fd_ < 0) {
    return;
  }

  (void)::write(log_fd_, kShutdownFooter, std::strlen(kShutdownFooter));
  (void)::fsync(log_fd_);

  consumeMarkerFile(markerPath(kCurrentSessionMarker));
  g_crash_paths_ready.store(false, std::memory_order_release);
  g_log_fd.store(-1, std::memory_order_release);
  (void)::close(log_fd_);
  log_fd_ = -1;
  started_ = false;
}

void SessionEventLog::log_ui(std::string_view action, std::string_view detail) {
  std::lock_guard lock(mutex_);
  if (!started_ || log_fd_ < 0) {
    return;
  }
  writeLineLocked(log_fd_, "UI", action, detail);
}

void SessionEventLog::log_backend(std::string_view action, std::string_view detail) {
  std::lock_guard lock(mutex_);
  if (!started_ || log_fd_ < 0) {
    return;
  }
  writeLineLocked(log_fd_, "BACKEND", action, detail);
}

std::filesystem::path SessionEventLog::session_path() const {
  std::lock_guard lock(mutex_);
  return session_path_;
}

bool SessionEventLog::active_session() const {
  std::lock_guard lock(mutex_);
  return started_;
}

std::optional<std::filesystem::path> SessionEventLog::take_previous_crash_log() {
  const auto logs_dir = logsDirectory();
  const auto last_crash_marker = logs_dir / kLastCrashMarker;
  if (const auto path = readMarkerPath(last_crash_marker)) {
    consumeMarkerFile(last_crash_marker);
    consumeMarkerFile(logs_dir / kCurrentSessionMarker);
    return path;
  }

  if (!instance().active_session()) {
    const auto current_marker = logs_dir / kCurrentSessionMarker;
    if (const auto path = readMarkerPath(current_marker)) {
      consumeMarkerFile(current_marker);
      std::ofstream marker(last_crash_marker);
      if (marker) {
        marker << utf8StringFromPath(*path) << '\n';
      }
      return path;
    }
  }

  return std::nullopt;
}

void SessionEventLog::debug_simulate_crash_footer(int signal) {
  std::lock_guard lock(mutex_);
  if (!started_ || log_fd_ < 0) {
    return;
  }
  writeCrashFooter(log_fd_, signal);
  writeCrashMarkerPath();
}

} // namespace video_editor::app

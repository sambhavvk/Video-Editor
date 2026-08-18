// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

namespace video_editor::app {

class SessionEventLog {
public:
  static SessionEventLog& instance();

  void start();
  void shutdown();

  void log_ui(std::string_view action, std::string_view detail = {});
  void log_backend(std::string_view action, std::string_view detail = {});

  [[nodiscard]] std::filesystem::path session_path() const;
  [[nodiscard]] bool active_session() const;

  [[nodiscard]] static std::optional<std::filesystem::path> take_previous_crash_log();
  static void install_crash_handlers();

  // Test hook: writes the same crash footer and marker as the signal handler.
  void debug_simulate_crash_footer(int signal = 11);

private:
  SessionEventLog() = default;

  mutable std::mutex mutex_;
  std::filesystem::path session_path_;
  int log_fd_{-1};
  bool started_{false};
};

} // namespace video_editor::app

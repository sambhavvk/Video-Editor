// SPDX-License-Identifier: MPL-2.0

#include "video_editor/job_service/framing.h"
#include "video_editor/job_service/job_id.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/proxy_service/proxy_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace video_editor::workers {
namespace {

namespace protocol = jobs::v1;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_worker_death_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string utf8_string(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::string shell_quote(const std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

[[nodiscard]] bool create_long_proxy_fixture(const std::filesystem::path& path) {
#ifndef VIDEO_EDITOR_WORKER_TEST_FFMPEG
  static_cast<void>(path);
  return false;
#else
  const std::vector<std::string> arguments{
      VIDEO_EDITOR_WORKER_TEST_FFMPEG,
      "-hide_banner",
      "-loglevel",
      "error",
      "-nostdin",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=320x180:rate=24:duration=8",
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=440:sample_rate=48000:duration=8",
      "-map",
      "0:v",
      "-map",
      "1:a",
      "-c:v",
      "mpeg4",
      "-q:v",
      "4",
      "-c:a",
      "pcm_s16le",
      path.string(),
  };
  std::ostringstream command;
  for (const std::string& argument : arguments) {
    if (command.tellp() > 0) {
      command << ' ';
    }
    command << shell_quote(argument);
  }
  return std::system(command.str().c_str()) == 0;
#endif
}

[[nodiscard]] protocol::JobSpec proxy_spec(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) {
  protocol::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(protocol::JOB_KIND_PROXY);
  spec.add_input_uris(utf8_string(source));
  spec.set_output_uri(utf8_string(destination));
  spec.set_preset_id("video-editor.proxy.ffv1-half.v1");
  return spec;
}

#ifndef _WIN32
class UniqueFd final {
public:
  explicit UniqueFd(const int fd = -1) : fd_(fd) {}
  ~UniqueFd() {
    reset();
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_;
};

class ChildProcess final {
public:
  explicit ChildProcess(const pid_t pid) : pid_(pid) {}
  ~ChildProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  void send_sigkill() const {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
    }
  }

  int wait() {
    int status = 0;
    if (pid_ > 0) {
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
    return status;
  }

private:
  pid_t pid_{-1};
};

[[nodiscard]] bool write_all(const int fd, const std::string& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}
#endif

TEST(WorkerHostDeath, SigkillBeforeCommitDoesNotLeaveCompleteProxy) {
#ifdef _WIN32
  GTEST_SKIP() << "worker-host SIGKILL injection is Linux-first";
#elif !defined(VIDEO_EDITOR_WORKER_HOST)
  GTEST_SKIP() << "worker host path is not configured";
#else
  const std::filesystem::path host_path{VIDEO_EDITOR_WORKER_HOST};
  if (!std::filesystem::is_regular_file(host_path)) {
    GTEST_SKIP() << "worker host binary is missing: " << host_path.string();
  }

  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_long_proxy_fixture(source)) {
    std::ofstream(source, std::ios::binary) << "not-a-complete-media-file";
  }
  const auto destination = directory.path() / "proxy.mkv";

  protocol::WorkerRequest request;
  request.set_protocol_major(jobs::kProtocolMajor);
  request.set_protocol_minor(jobs::kProtocolMinor);
  *request.mutable_start()->mutable_spec() = proxy_spec(source, destination);
  std::ostringstream framed;
  ASSERT_TRUE(jobs::write_frame(framed, request).ok);
  const std::string payload = framed.str();

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  ASSERT_EQ(::pipe(stdin_pipe), 0);
  UniqueFd child_stdin(stdin_pipe[0]);
  UniqueFd parent_stdin(stdin_pipe[1]);
  ASSERT_EQ(::pipe(stdout_pipe), 0);
  UniqueFd parent_stdout(stdout_pipe[0]);
  UniqueFd child_stdout(stdout_pipe[1]);

  posix_spawn_file_actions_t actions;
  ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
  const int add_stdin = posix_spawn_file_actions_adddup2(&actions, child_stdin.get(), STDIN_FILENO);
  const int add_stdout =
      posix_spawn_file_actions_adddup2(&actions, child_stdout.get(), STDOUT_FILENO);
  const int close_parent_in =
      posix_spawn_file_actions_addclose(&actions, parent_stdin.get());
  const int close_parent_out =
      posix_spawn_file_actions_addclose(&actions, parent_stdout.get());
  if (add_stdin != 0 || add_stdout != 0 || close_parent_in != 0 || close_parent_out != 0) {
    posix_spawn_file_actions_destroy(&actions);
    FAIL() << "posix_spawn_file_actions setup failed";
  }

  pid_t pid = 0;
  const char* argv[] = {VIDEO_EDITOR_WORKER_HOST, nullptr};
  const int spawned =
      posix_spawn(&pid, VIDEO_EDITOR_WORKER_HOST, &actions, nullptr,
                  const_cast<char**>(argv), environ);
  posix_spawn_file_actions_destroy(&actions);
  ASSERT_EQ(spawned, 0) << "posix_spawn failed: " << spawned;
  ChildProcess child(pid);
  child_stdin.reset();
  child_stdout.reset();

  ASSERT_TRUE(write_all(parent_stdin.get(), payload));
  child.send_sigkill();
  const int status = child.wait();
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);

  const auto pts_map = proxy::default_pts_map_path(destination);
  const bool complete_proxy =
      std::filesystem::is_regular_file(destination) && proxy::load_pts_map(pts_map).has_value();
  EXPECT_FALSE(complete_proxy)
      << "SIGKILL before commit must not leave a complete proxy at " << destination.string();
  if (std::filesystem::exists(destination)) {
    EXPECT_FALSE(proxy::load_pts_map(pts_map).has_value());
  }
#endif
}

} // namespace
} // namespace video_editor::workers

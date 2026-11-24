/**
 * @file src/platform/linux/display_helper.cpp
 * @brief Fork/execv JSON stdin/stdout runner for Unix display helpers.
 */
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/logging.h"
#include "src/stream_desk_display.h"

namespace {

  std::string helper_path_from_env() {
    const char *env = std::getenv("STREAM_DESK_DISPLAY_HELPER");
    if (env == nullptr || env[0] == '\0') {
      return {};
    }
    return std::string(env);
  }

  bool create_cloexec_pipe(int fds[2]) {
    if (::pipe(fds) != 0) {
      return false;
    }
    if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      fds[0] = fds[1] = -1;
      return false;
    }
    return true;
  }

  bool invoke_helper_json(const std::string &path,
                          const nlohmann::json &request,
                          nlohmann::json &response,
                          int timeout_ms) {
    if (path.empty() || ::access(path.c_str(), X_OK) != 0) {
      return false;
    }

    int in_pipe[2] {-1, -1};
    int out_pipe[2] {-1, -1};
    if (!create_cloexec_pipe(in_pipe)) {
      return false;
    }
    if (!create_cloexec_pipe(out_pipe)) {
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      return false;
    }

    if (pid == 0) {
      // Child: bind pipes cleanly to stdin/stdout; extra CLOEXEC fds close on execv
      ::dup2(in_pipe[0], STDIN_FILENO);
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::close(in_pipe[0]);
      ::close(in_pipe[1]);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);

      char *argv[] = {const_cast<char *>(path.c_str()), nullptr};
      ::execv(path.c_str(), argv);
      ::_exit(127);
    }

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);
    ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    const std::string payload = request.dump() + "\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::size_t written = 0;
    while (written < payload.size()) {
      auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (rem <= 0) {
        break;
      }
      struct pollfd pfd {in_pipe[1], POLLOUT, 0};
      int pr = ::poll(&pfd, 1, static_cast<int>(rem));
      if (pr <= 0) {
        if (pr < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      ssize_t n = ::write(in_pipe[1], payload.data() + written, payload.size() - written);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        break;
      }
      written += static_cast<std::size_t>(n);
    }
    ::close(in_pipe[1]);

    std::string stdout_buf;
    constexpr std::size_t kMaxStdoutBytes = 65536;
    char buf[4096];
    bool read_eof = false;
    bool timed_out = false;

    while (!read_eof) {
      auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (rem <= 0) {
        timed_out = true;
        break;
      }
      struct pollfd pfd {out_pipe[0], POLLIN, 0};
      int pr = ::poll(&pfd, 1, static_cast<int>(rem));
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (pr == 0) {
        timed_out = true;
        break;
      }

      ssize_t n = ::read(out_pipe[0], buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        break;
      }
      if (n == 0) {
        read_eof = true;
        break;
      }
      if (stdout_buf.size() + static_cast<std::size_t>(n) > kMaxStdoutBytes) {
        timed_out = true;
        break;
      }
      stdout_buf.append(buf, static_cast<std::size_t>(n));
    }
    ::close(out_pipe[0]);

    if (timed_out) {
      ::kill(pid, SIGKILL);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return false;
    }

    response = nlohmann::json::parse(stdout_buf, nullptr, false);
    return !response.is_discarded() && response.is_object();
  }

  class unix_display_backend_t final: public stream_desk::display_backend_t {
  public:
    stream_desk::display_caps_t probe() override {
      const std::string path = helper_path_from_env();
      if (path.empty()) {
        return {};
      }
      nlohmann::json req = {
        {"version", 1},
        {"action", "probe"},
      };
      nlohmann::json resp;
      if (!invoke_helper_json(path, req, resp, 200)) {
        return {};
      }
      if (!resp.value("ok", false)) {
        return {};
      }
      stream_desk::display_caps_t caps;
      caps.adaptive_display = resp.value("adaptiveDisplay", false);
      caps.virtual_display = resp.value("virtualDisplay", false);
      return caps;
    }

    bool configure(const stream_desk::session_display_request_t &req_data, std::string &output_name) override {
      const std::string path = helper_path_from_env();
      if (path.empty()) {
        return false;
      }
      nlohmann::json req = {
        {"version", 1},
        {"action", "configure"},
        {"sessionId", req_data.session_id},
        {"width", req_data.width},
        {"height", req_data.height},
        {"fps", req_data.fps},
        {"desktopScale", req_data.desktop_scale},
        {"virtualDisplay", req_data.virtual_display},
      };
      nlohmann::json resp;
      if (!invoke_helper_json(path, req, resp, 2000) || !resp.value("ok", false)) {
        return false;
      }
      output_name = resp.value("outputName", std::string {});
      return true;
    }

    bool revert() override {
      const std::string path = helper_path_from_env();
      if (path.empty()) {
        return true;
      }
      nlohmann::json req = {
        {"version", 1},
        {"action", "revert"},
      };
      nlohmann::json resp;
      return invoke_helper_json(path, req, resp, 2000) && resp.value("ok", false);
    }
  };

}  // namespace

namespace platf {

  std::unique_ptr<stream_desk::display_backend_t> create_display_helper_backend() {
    return std::make_unique<unix_display_backend_t>();
  }

}  // namespace platf

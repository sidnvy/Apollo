/**
 * @file src/platform/linux/commit_text_fcitx.cpp
 * @brief Bounded AF_UNIX client to fcitx5-remote-commit addon.
 */
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "src/stream_desk.h"

namespace {

  constexpr std::uint32_t kProtoMagic = 0x53444354u;  // "SDCT"
  constexpr std::size_t kHeaderSize = 12;
  constexpr std::uint8_t kCmdPing = 0x01;
  constexpr std::uint8_t kCmdCommitText = 0x02;
  constexpr std::uint8_t kStatusOk = 0x00;

  std::string resolve_socket_path() {
    if (const char *override_path = std::getenv("STREAM_DESK_FCITX5_SOCKET")) {
      if (override_path[0] != '\0') {
        return std::string(override_path);
      }
    }
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR")) {
      if (xdg[0] != '\0') {
        return std::string(xdg) + "/stream-desk-fcitx5-commit.sock";
      }
    }
    return "/tmp/stream-desk-fcitx5-commit-" + std::to_string(static_cast<unsigned long>(::getuid())) + ".sock";
  }

  void write_be32(std::uint8_t *dst, std::uint32_t v) {
    dst[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>(v & 0xFFu);
  }

  bool send_request_with_timeout(std::uint8_t cmd, const std::string &payload, int timeout_ms) {
    const std::string path = resolve_socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
      return false;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return false;
    }

    struct fd_guard_t {
      int fd;
      ~fd_guard_t() {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    } guard {fd};

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto remaining_ms = [&]() -> int {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      return ms > 0 ? static_cast<int>(ms) : 0;
    };

    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
      if (errno != EINPROGRESS) {
        return false;
      }
      struct pollfd pfd {fd, POLLOUT, 0};
      int rem = remaining_ms();
      if (rem <= 0 || ::poll(&pfd, 1, rem) <= 0) {
        return false;
      }
      int so_err = 0;
      socklen_t len = sizeof(so_err);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0 || so_err != 0) {
        return false;
      }
    }

    std::vector<std::uint8_t> packet(kHeaderSize + payload.size());
    write_be32(packet.data(), kProtoMagic);
    packet[4] = cmd;
    packet[5] = 0;
    packet[6] = 0;
    packet[7] = 0;
    write_be32(packet.data() + 8, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
      std::memcpy(packet.data() + kHeaderSize, payload.data(), payload.size());
    }

    std::size_t off = 0;
    while (off < packet.size()) {
      struct pollfd pfd {fd, POLLOUT, 0};
      int rem = remaining_ms();
      if (rem <= 0 || ::poll(&pfd, 1, rem) <= 0) {
        return false;
      }
      ssize_t n = ::send(fd, packet.data() + off, packet.size() - off, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        return false;
      }
      off += static_cast<std::size_t>(n);
    }

    struct pollfd pfd {fd, POLLIN, 0};
    int rem = remaining_ms();
    if (rem <= 0 || ::poll(&pfd, 1, rem) <= 0) {
      return false;
    }

    std::uint8_t status = 0xFF;
    ssize_t r = ::recv(fd, &status, 1, 0);
    return r == 1 && status == kStatusOk;
  }

}  // namespace

namespace platf {

  bool commit_text_available() {
    return send_request_with_timeout(kCmdPing, "", 100);
  }

  bool commit_text(const std::string &utf8) {
    if (utf8.empty() || utf8.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
      return false;
    }
    return send_request_with_timeout(kCmdCommitText, utf8, 150);
  }

}  // namespace platf

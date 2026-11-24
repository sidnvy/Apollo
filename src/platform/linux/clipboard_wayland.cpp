/**
 * @file src/platform/linux/clipboard_wayland.cpp
 * @brief Event-driven Wayland ext-data-control-v1 clipboard bridge for Linux.
 */
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <wayland-client.h>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/StreamDesk.h>
}

#include "ext-data-control-v1.h"
#include "src/logging.h"
#include "src/stream_desk.h"

namespace platf {
  std::string wayland_get_clipboard();
  bool wayland_set_clipboard(const std::string &content);
}  // namespace platf

namespace {

  std::mutex g_clipboard_cache_mutex;
  std::string g_last_clipboard_text;
  std::atomic<platf::clipboard_watcher_t *> g_active_watcher {nullptr};

  void set_cached_clipboard_text(const std::string &text) {
    std::lock_guard<std::mutex> lg(g_clipboard_cache_mutex);
    g_last_clipboard_text = text;
  }

  std::string get_cached_clipboard_text() {
    std::lock_guard<std::mutex> lg(g_clipboard_cache_mutex);
    return g_last_clipboard_text;
  }

  struct probe_globals_t {
    bool has_seat {false};
    bool has_data_control {false};
  };

  void on_probe_global(void *data,
                       wl_registry *,
                       uint32_t,
                       const char *interface,
                       uint32_t) {
    auto *p = static_cast<probe_globals_t *>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      p->has_seat = true;
    } else if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
      p->has_data_control = true;
    }
  }

  void on_probe_global_remove(void *, wl_registry *, uint32_t) {}

  constexpr wl_registry_listener kProbeRegistryListener = {
    .global = on_probe_global,
    .global_remove = on_probe_global_remove,
  };

  int mime_priority(const char *mime) {
    if (std::strcmp(mime, "text/plain;charset=utf-8") == 0) {
      return 3;
    }
    if (std::strcmp(mime, "UTF8_STRING") == 0) {
      return 2;
    }
    if (std::strcmp(mime, "text/plain") == 0) {
      return 1;
    }
    return 0;
  }

  bool read_fd_bounded(int fd, std::string &out_text, std::size_t max_bytes, int timeout_ms) {
    out_text.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];

    for (;;) {
      auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (rem <= 0) {
        return false;
      }

      struct pollfd pfd {fd, POLLIN, 0};
      int pr = ::poll(&pfd, 1, static_cast<int>(rem));
      if (pr < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (pr == 0) {
        return false;
      }

      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        return false;
      }
      if (n == 0) {
        break;
      }
      if (out_text.size() + static_cast<std::size_t>(n) > max_bytes) {
        out_text.clear();
        return false;
      }
      out_text.append(buf, static_cast<std::size_t>(n));
    }

    return true;
  }

  void write_all_best_effort(int fd, const std::string &text) {
    std::size_t off = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (off < text.size()) {
      auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (rem <= 0) {
        break;
      }
      struct pollfd pfd {fd, POLLOUT, 0};
      int pr = ::poll(&pfd, 1, static_cast<int>(rem));
      if (pr <= 0) {
        if (pr < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        break;
      }
      ssize_t n = ::write(fd, text.data() + off, text.size() - off);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        if (errno == EPIPE) {
          // Drain any thread-directed SIGPIPE while blocked so paste-pipe aborts never kill Apollo
          sigset_t sp;
          sigemptyset(&sp);
          sigaddset(&sp, SIGPIPE);
          struct timespec z {0, 0};
          ::sigtimedwait(&sp, nullptr, &z);
        }
        break;
      }
      off += static_cast<std::size_t>(n);
    }
  }

  class wayland_clipboard_watcher_t final: public platf::clipboard_watcher_t {
  public:
    explicit wayland_clipboard_watcher_t(platf::host_clipboard_cb_t on_host_clipboard):
        on_host_clipboard_(std::move(on_host_clipboard)),
        wake_efd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
      g_active_watcher.store(this, std::memory_order_release);
      worker_ = std::thread(&wayland_clipboard_watcher_t::run, this);
    }

    ~wayland_clipboard_watcher_t() override {
      stop();
    }

    bool set_text(const std::string &utf8, std::uint64_t content_hash) override {
      if (utf8.empty() || utf8.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
        return false;
      }

      dedup_.record_client_push(content_hash);
      set_cached_clipboard_text(utf8);

      {
        std::lock_guard<std::mutex> lg(out_mutex_);
        pending_selections_.push_back(utf8);
      }
      signal_wakeup();
      return true;
    }

    void stop() {
      platf::clipboard_watcher_t *expected = this;
      g_active_watcher.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

      if (!stop_.exchange(true)) {
        signal_wakeup();
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      if (wake_efd_ >= 0) {
        ::close(wake_efd_);
        wake_efd_ = -1;
      }
    }

  private:
    struct offer_info_t {
      wayland_clipboard_watcher_t *self {nullptr};
      std::string best_mime;
      int best_priority {0};
      bool is_self {false};
    };

    struct source_ctx_t {
      wayland_clipboard_watcher_t *self {nullptr};
      ext_data_control_source_v1 *source {nullptr};
      std::string payload;
    };

    void signal_wakeup() const {
      if (wake_efd_ >= 0) {
        std::uint64_t one = 1;
        ssize_t ignored = ::write(wake_efd_, &one, sizeof(one));
        (void)ignored;
      }
    }

    void drain_wakeup() const {
      if (wake_efd_ >= 0) {
        std::uint64_t val = 0;
        while (::read(wake_efd_, &val, sizeof(val)) > 0) {}
      }
    }

    static void on_global(void *data,
                          wl_registry *reg,
                          uint32_t id,
                          const char *interface,
                          uint32_t version) {
      auto *self = static_cast<wayland_clipboard_watcher_t *>(data);
      if (std::strcmp(interface, wl_seat_interface.name) == 0 && self->seat_ == nullptr) {
        self->seat_ = static_cast<wl_seat *>(wl_registry_bind(reg, id, &wl_seat_interface, 1));
      } else if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 && self->manager_ == nullptr) {
        self->manager_ = static_cast<ext_data_control_manager_v1 *>(
          wl_registry_bind(reg, id, &ext_data_control_manager_v1_interface, std::min<uint32_t>(version, 1)));
      }
    }

    static void on_global_remove(void *, wl_registry *, uint32_t) {}

    static void on_offer_mime(void *data, ext_data_control_offer_v1 *, const char *mime_type) {
      auto *info = static_cast<offer_info_t *>(data);
      if (info == nullptr || mime_type == nullptr) {
        return;
      }
      if (info->self != nullptr && std::strcmp(mime_type, info->self->self_mime_.c_str()) == 0) {
        info->is_self = true;
        return;
      }
      int prio = mime_priority(mime_type);
      if (prio > info->best_priority) {
        info->best_priority = prio;
        info->best_mime = mime_type;
      }
    }

    static void on_device_data_offer(void *data,
                                     ext_data_control_device_v1 *,
                                     ext_data_control_offer_v1 *offer) {
      auto *self = static_cast<wayland_clipboard_watcher_t *>(data);
      static constexpr ext_data_control_offer_v1_listener kOfferListener = {
        .offer = on_offer_mime,
      };
      auto info = std::make_unique<offer_info_t>();
      info->self = self;
      ext_data_control_offer_v1_add_listener(offer, &kOfferListener, info.get());
      self->pending_offers_.emplace(offer, std::move(info));
    }

    static void on_device_selection(void *data,
                                    ext_data_control_device_v1 *,
                                    ext_data_control_offer_v1 *offer) {
      auto *self = static_cast<wayland_clipboard_watcher_t *>(data);
      if (offer == nullptr) {
        return;
      }

      auto it = self->pending_offers_.find(offer);
      std::unique_ptr<offer_info_t> info;
      if (it != self->pending_offers_.end()) {
        info = std::move(it->second);
        self->pending_offers_.erase(it);
      }

      if (info && info->is_self) {
        ext_data_control_offer_v1_destroy(offer);
        return;
      }

      if (info && !info->best_mime.empty() && self->display_ != nullptr) {
        int pipe_fds[2] {-1, -1};
        if (::pipe2(pipe_fds, O_CLOEXEC) == 0) {
          ::fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
          ::fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
          ext_data_control_offer_v1_receive(offer, info->best_mime.c_str(), pipe_fds[1]);
          ::close(pipe_fds[1]);
          wl_display_flush(self->display_);

          std::string text;
          if (read_fd_bounded(pipe_fds[0], text, LI_STREAM_DESK_MAX_TEXT_BYTES, 200) && !text.empty()) {
            set_cached_clipboard_text(text);
            const std::uint64_t hash = LiStreamDeskComputeClipboardHash(text.data(), static_cast<std::uint32_t>(text.size()));
            if (self->dedup_.should_forward_host_event(hash) && self->on_host_clipboard_) {
              self->on_host_clipboard_(text, hash, 0);
            }
          }
          ::close(pipe_fds[0]);
        }
      }

      ext_data_control_offer_v1_destroy(offer);
    }

    static void on_device_finished(void *data, ext_data_control_device_v1 *) {
      auto *self = static_cast<wayland_clipboard_watcher_t *>(data);
      self->stop_ = true;
    }

    static void on_device_primary_selection(void *data,
                                            ext_data_control_device_v1 *,
                                            ext_data_control_offer_v1 *offer) {
      auto *self = static_cast<wayland_clipboard_watcher_t *>(data);
      if (offer != nullptr) {
        self->pending_offers_.erase(offer);
        ext_data_control_offer_v1_destroy(offer);
      }
    }

    static void on_source_send(void *data,
                               ext_data_control_source_v1 *,
                               const char *,
                               int32_t fd) {
      auto *ctx = static_cast<source_ctx_t *>(data);
      if (ctx != nullptr && fd >= 0) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        write_all_best_effort(fd, ctx->payload);
      }
      if (fd >= 0) {
        ::close(fd);
      }
    }

    static void on_source_cancelled(void *data, ext_data_control_source_v1 *source) {
      auto *ctx = static_cast<source_ctx_t *>(data);
      if (ctx != nullptr && ctx->self != nullptr) {
        ext_data_control_source_v1_destroy(source);
        ctx->self->active_sources_.erase(source);
      }
    }

    void publish_pending_selections() {
      std::deque<std::string> batch;
      {
        std::lock_guard<std::mutex> lg(out_mutex_);
        batch.swap(pending_selections_);
      }
      if (batch.empty() || manager_ == nullptr || device_ == nullptr) {
        return;
      }

      // Only the latest clipboard content matters for selection ownership
      std::string latest = std::move(batch.back());

      static constexpr ext_data_control_source_v1_listener kSourceListener = {
        .send = on_source_send,
        .cancelled = on_source_cancelled,
      };

      ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source(manager_);
      if (source == nullptr) {
        return;
      }

      auto ctx = std::make_unique<source_ctx_t>();
      ctx->self = this;
      ctx->source = source;
      ctx->payload = std::move(latest);

      ext_data_control_source_v1_add_listener(source, &kSourceListener, ctx.get());
      ext_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
      ext_data_control_source_v1_offer(source, "UTF8_STRING");
      ext_data_control_source_v1_offer(source, "text/plain");
      ext_data_control_source_v1_offer(source, self_mime_.c_str());
      ext_data_control_device_v1_set_selection(device_, source);
      active_sources_.emplace(source, std::move(ctx));
      wl_display_flush(display_);
    }

    void run() {
      sigset_t pipe_mask;
      sigemptyset(&pipe_mask);
      sigaddset(&pipe_mask, SIGPIPE);
      pthread_sigmask(SIG_BLOCK, &pipe_mask, nullptr);

      const char *disp_env = std::getenv("WAYLAND_DISPLAY");
      if (disp_env == nullptr || disp_env[0] == '\0') {
        return;
      }

      display_ = wl_display_connect(disp_env);
      if (display_ == nullptr) {
        BOOST_LOG(warning) << "Wayland clipboard watcher failed to connect to WAYLAND_DISPLAY=" << disp_env;
        return;
      }

      static constexpr wl_registry_listener kRegListener = {
        .global = on_global,
        .global_remove = on_global_remove,
      };

      wl_registry *registry = wl_display_get_registry(display_);
      wl_registry_add_listener(registry, &kRegListener, this);
      wl_display_roundtrip(display_);

      if (seat_ != nullptr && manager_ != nullptr) {
        static constexpr ext_data_control_device_v1_listener kDeviceListener = {
          .data_offer = on_device_data_offer,
          .selection = on_device_selection,
          .finished = on_device_finished,
          .primary_selection = on_device_primary_selection,
        };
        device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
        if (device_ != nullptr) {
          ext_data_control_device_v1_add_listener(device_, &kDeviceListener, this);
          wl_display_roundtrip(display_);
        }
      }

      if (device_ != nullptr) {
        const int wl_fd = wl_display_get_fd(display_);
        while (!stop_.load()) {
          publish_pending_selections();

          while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) {
              stop_ = true;
              break;
            }
          }
          if (stop_.load()) {
            wl_display_cancel_read(display_);
            break;
          }

          if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display_);
            break;
          }

          struct pollfd pfds[2] {};
          pfds[0].fd = wl_fd;
          pfds[0].events = POLLIN;
          pfds[1].fd = wake_efd_;
          pfds[1].events = POLLIN;

          int pr = ::poll(pfds, 2, -1);
          if (pr < 0) {
            wl_display_cancel_read(display_);
            if (errno == EINTR) {
              continue;
            }
            break;
          }

          if (pfds[0].revents & POLLIN) {
            if (wl_display_read_events(display_) < 0) {
              break;
            }
            if (wl_display_dispatch_pending(display_) < 0) {
              break;
            }
          } else {
            wl_display_cancel_read(display_);
          }

          if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
          }
          if (pfds[1].revents & POLLIN) {
            drain_wakeup();
          }
        }
      }

      for (auto &[offer, _] : pending_offers_) {
        ext_data_control_offer_v1_destroy(offer);
      }
      pending_offers_.clear();

      for (auto &[src, _] : active_sources_) {
        ext_data_control_source_v1_destroy(src);
      }
      active_sources_.clear();

      if (device_ != nullptr) {
        ext_data_control_device_v1_destroy(device_);
        device_ = nullptr;
      }
      if (manager_ != nullptr) {
        ext_data_control_manager_v1_destroy(manager_);
        manager_ = nullptr;
      }
      if (seat_ != nullptr) {
        wl_seat_destroy(seat_);
        seat_ = nullptr;
      }
      wl_registry_destroy(registry);
      wl_display_disconnect(display_);
      display_ = nullptr;
    }

    platf::host_clipboard_cb_t on_host_clipboard_;
    int wake_efd_ {-1};
    std::atomic<bool> stop_ {false};
    std::thread worker_;

    wl_display *display_ {nullptr};
    wl_seat *seat_ {nullptr};
    ext_data_control_manager_v1 *manager_ {nullptr};
    ext_data_control_device_v1 *device_ {nullptr};

    stream_desk::clipboard_dedup_t dedup_;
    std::string self_mime_ {"application/x-stream-desk-owner-" + std::to_string(static_cast<unsigned long>(::getpid())) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this))};
    std::mutex out_mutex_;
    std::deque<std::string> pending_selections_;
    std::unordered_map<ext_data_control_offer_v1 *, std::unique_ptr<offer_info_t>> pending_offers_;
    std::unordered_map<ext_data_control_source_v1 *, std::unique_ptr<source_ctx_t>> active_sources_;
  };

}  // namespace

namespace platf {

  bool clipboard_event_available() {
    const char *disp_env = std::getenv("WAYLAND_DISPLAY");
    if (disp_env == nullptr || disp_env[0] == '\0') {
      return false;
    }

    wl_display *display = wl_display_connect(disp_env);
    if (display == nullptr) {
      return false;
    }

    probe_globals_t probe {};
    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kProbeRegistryListener, &probe);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return probe.has_seat && probe.has_data_control;
  }

  std::unique_ptr<clipboard_watcher_t> create_clipboard_watcher(host_clipboard_cb_t on_host_clipboard) {
    if (!clipboard_event_available()) {
      return nullptr;
    }
    return std::make_unique<wayland_clipboard_watcher_t>(std::move(on_host_clipboard));
  }

  std::string wayland_get_clipboard() {
    return get_cached_clipboard_text();
  }

  bool wayland_set_clipboard(const std::string &content) {
    if (content.empty() || content.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
      return false;
    }
    if (auto *watcher = g_active_watcher.load(std::memory_order_acquire)) {
      const std::uint64_t hash = LiStreamDeskComputeClipboardHash(content.data(), static_cast<std::uint32_t>(content.size()));
      return watcher->set_text(content, hash);
    }
    return false;
  }

}  // namespace platf

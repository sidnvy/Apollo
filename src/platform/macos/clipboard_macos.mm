/**
 * @file src/platform/macos/clipboard_macos.mm
 * @brief macOS NSPasteboard changeCount-based clipboard watcher.
 */
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "src/stream_desk.h"

namespace {

  class macos_clipboard_watcher_t final : public platf::clipboard_watcher_t {
  public:
    explicit macos_clipboard_watcher_t(platf::host_clipboard_cb_t on_host_clipboard):
        on_host_clipboard_(std::move(on_host_clipboard)) {
      @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        last_change_count_ = pb.changeCount;
      }
      worker_ = std::thread([this]() {
        watch_loop();
      });
    }

    ~macos_clipboard_watcher_t() override {
      {
        std::lock_guard<std::mutex> lg(sleep_mutex_);
        running_.store(false, std::memory_order_release);
      }
      sleep_cv_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
    }

    bool set_text(const std::string &utf8, std::uint64_t content_hash) override {
      if (utf8.empty() || utf8.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
        return false;
      }
      if (!dedup_.should_apply_client_push(content_hash)) {
        return true;
      }

      @autoreleasepool {
        NSString *text = [[NSString alloc] initWithBytes:utf8.data()
                                                  length:utf8.size()
                                                encoding:NSUTF8StringEncoding];
        if (text == nil) {
          return false;
        }

        std::lock_guard<std::mutex> pb_lock(pb_mutex_);
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        const BOOL ok = [pb setString:text forType:NSPasteboardTypeString];
#if !__has_feature(objc_arc)
        [text release];
#endif
        if (!ok) {
          return false;
        }

        last_change_count_ = pb.changeCount;
        dedup_.record_client_push(content_hash);
        return true;
      }
    }

  private:
    void watch_loop() {
      while (running_.load(std::memory_order_acquire)) {
        std::string changed_utf8;
        std::uint64_t changed_hash = 0;

        @autoreleasepool {
          std::lock_guard<std::mutex> pb_lock(pb_mutex_);
          NSPasteboard *pb = [NSPasteboard generalPasteboard];
          const NSInteger count = pb.changeCount;
          if (count != last_change_count_) {
            last_change_count_ = count;
            NSString *str = [pb stringForType:NSPasteboardTypeString];
            if (str != nil) {
              const char *raw = [str UTF8String];
              if (raw != nullptr) {
                const std::size_t len = std::char_traits<char>::length(raw);
                if (len > 0 && len <= LI_STREAM_DESK_MAX_TEXT_BYTES) {
                  changed_hash = LiStreamDeskComputeClipboardHash(
                    raw,
                    static_cast<std::uint32_t>(len)
                  );
                  if (dedup_.should_forward_host_event(changed_hash)) {
                    changed_utf8.assign(raw, len);
                  }
                }
              }
            }
          }
        }

        if (!changed_utf8.empty() && on_host_clipboard_) {
          on_host_clipboard_(changed_utf8, changed_hash, 0);
        }

        std::unique_lock<std::mutex> sleep_lock(sleep_mutex_);
        sleep_cv_.wait_for(sleep_lock, std::chrono::milliseconds(150), [this]() {
          return !running_.load(std::memory_order_acquire);
        });
      }
    }

    platf::host_clipboard_cb_t on_host_clipboard_;
    stream_desk::clipboard_dedup_t dedup_;
    std::mutex pb_mutex_;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
    std::atomic<bool> running_ {true};
    NSInteger last_change_count_ {0};
    std::thread worker_;
  };

}  // namespace

namespace platf {

  bool clipboard_event_available() {
    return true;
  }

  std::unique_ptr<clipboard_watcher_t> create_clipboard_watcher(host_clipboard_cb_t on_host_clipboard) {
    return std::make_unique<macos_clipboard_watcher_t>(std::move(on_host_clipboard));
  }

}  // namespace platf

/**
 * @file src/platform/windows/clipboard_win.cpp
 * @brief Windows AddClipboardFormatListener event-driven clipboard watcher.
 */
#define WINVER 0x0A00

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "src/logging.h"
#include "src/stream_desk.h"

namespace {

  constexpr UINT WM_STREAM_DESK_SET_CLIPBOARD = WM_APP + 1;
  constexpr wchar_t kClipboardWindowClass[] = L"StreamDeskClipboardWatcherWindow";

  std::string wide_to_utf8(const wchar_t *src) {
    if (src == nullptr || src[0] == L'\0') {
      return {};
    }
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 1) {
      return {};
    }
    std::string out(static_cast<std::size_t>(utf8_len - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), utf8_len - 1, nullptr, nullptr) <= 0) {
      return {};
    }
    return out;
  }

  std::wstring utf8_to_wide(const std::string &src) {
    if (src.empty()) {
      return {};
    }
    const int wide_len = MultiByteToWideChar(
      CP_UTF8,
      0,
      src.data(),
      static_cast<int>(src.size()),
      nullptr,
      0
    );
    if (wide_len <= 0) {
      return {};
    }
    std::wstring out(static_cast<std::size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(
          CP_UTF8,
          0,
          src.data(),
          static_cast<int>(src.size()),
          out.data(),
          wide_len
        ) != wide_len) {
      return {};
    }
    return out;
  }

  class win_clipboard_watcher_t final : public platf::clipboard_watcher_t {
  public:
    explicit win_clipboard_watcher_t(platf::host_clipboard_cb_t on_host_clipboard):
        on_host_clipboard_(std::move(on_host_clipboard)) {
      std::promise<bool> ready_promise;
      auto ready_future = ready_promise.get_future();
      worker_ = std::thread([this, p = std::move(ready_promise)]() mutable {
        thread_main(std::move(p));
      });
      ready_future.wait();
    }

    ~win_clipboard_watcher_t() override {
      HWND target = nullptr;
      {
        std::lock_guard<std::mutex> lg(state_mutex_);
        target = hwnd_;
      }
      if (target != nullptr) {
        PostMessageW(target, WM_CLOSE, 0, 0);
      }
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

      HWND target = nullptr;
      {
        std::lock_guard<std::mutex> lg(state_mutex_);
        if (hwnd_ == nullptr) {
          return false;
        }
        pending_text_ = utf8;
        has_pending_text_ = true;
        target = hwnd_;
      }

      dedup_.record_client_push(content_hash);
      return PostMessageW(target, WM_STREAM_DESK_SET_CLIPBOARD, 0, 0) != FALSE;
    }

  private:
    static void ensure_window_class(HINSTANCE instance) {
      static std::once_flag once;
      std::call_once(once, [instance]() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &win_clipboard_watcher_t::wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = kClipboardWindowClass;
        RegisterClassExW(&wc);
      });
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      auto *self = reinterpret_cast<win_clipboard_watcher_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (msg == WM_NCCREATE) {
        const auto *cs = reinterpret_cast<const CREATESTRUCTW *>(lp);
        self = static_cast<win_clipboard_watcher_t *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }
      if (self != nullptr) {
        return self->handle_message(hwnd, msg, wp, lp);
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool open_clipboard_with_retry(HWND hwnd) {
      for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(hwnd)) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return false;
    }

    void handle_host_clipboard_update(HWND hwnd) {
      if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return;
      }
      if (!open_clipboard_with_retry(hwnd)) {
        return;
      }

      std::string utf8;
      HANDLE data = GetClipboardData(CF_UNICODETEXT);
      if (data != nullptr) {
        const auto *locked = static_cast<const wchar_t *>(GlobalLock(data));
        if (locked != nullptr) {
          utf8 = wide_to_utf8(locked);
          GlobalUnlock(data);
        }
      }
      CloseClipboard();

      if (utf8.empty() || utf8.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
        return;
      }

      const std::uint64_t hash = LiStreamDeskComputeClipboardHash(
        utf8.data(),
        static_cast<std::uint32_t>(utf8.size())
      );
      if (!dedup_.should_forward_host_event(hash)) {
        return;
      }
      if (on_host_clipboard_) {
        on_host_clipboard_(utf8, hash, 0);
      }
    }

    void apply_pending_clipboard(HWND hwnd) {
      std::string utf8;
      {
        std::lock_guard<std::mutex> lg(state_mutex_);
        if (!has_pending_text_) {
          return;
        }
        utf8 = std::move(pending_text_);
        pending_text_.clear();
        has_pending_text_ = false;
      }

      const std::wstring wide = utf8_to_wide(utf8);
      if (wide.empty()) {
        return;
      }

      const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
      HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (mem == nullptr) {
        return;
      }
      void *dst = GlobalLock(mem);
      if (dst == nullptr) {
        GlobalFree(mem);
        return;
      }
      std::memcpy(dst, wide.c_str(), bytes);
      GlobalUnlock(mem);

      if (!open_clipboard_with_retry(hwnd)) {
        GlobalFree(mem);
        return;
      }
      EmptyClipboard();
      if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        GlobalFree(mem);
      }
      CloseClipboard();
    }

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      switch (msg) {
        case WM_CLIPBOARDUPDATE:
          handle_host_clipboard_update(hwnd);
          return 0;
        case WM_STREAM_DESK_SET_CLIPBOARD:
          apply_pending_clipboard(hwnd);
          return 0;
        case WM_CLOSE:
          RemoveClipboardFormatListener(hwnd);
          DestroyWindow(hwnd);
          return 0;
        case WM_DESTROY:
          PostQuitMessage(0);
          return 0;
        default:
          return DefWindowProcW(hwnd, msg, wp, lp);
      }
    }

    void thread_main(std::promise<bool> ready) {
      HINSTANCE instance = GetModuleHandleW(nullptr);
      ensure_window_class(instance);

      HWND hwnd = CreateWindowExW(
        0,
        kClipboardWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        this
      );
      if (hwnd == nullptr) {
        BOOST_LOG(warning) << "Windows clipboard CreateWindowExW failed: " << GetLastError();
        ready.set_value(false);
        return;
      }

      if (!AddClipboardFormatListener(hwnd)) {
        BOOST_LOG(warning) << "AddClipboardFormatListener failed: " << GetLastError();
        DestroyWindow(hwnd);
        ready.set_value(false);
        return;
      }

      {
        std::lock_guard<std::mutex> lg(state_mutex_);
        hwnd_ = hwnd;
      }
      ready.set_value(true);

      MSG msg {};
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      {
        std::lock_guard<std::mutex> lg(state_mutex_);
        hwnd_ = nullptr;
      }
    }

    platf::host_clipboard_cb_t on_host_clipboard_;
    stream_desk::clipboard_dedup_t dedup_;
    std::mutex state_mutex_;
    HWND hwnd_ {nullptr};
    std::string pending_text_;
    bool has_pending_text_ {false};
    std::thread worker_;
  };

}  // namespace

namespace platf {

  bool clipboard_event_available() {
    return true;
  }

  std::unique_ptr<clipboard_watcher_t> create_clipboard_watcher(host_clipboard_cb_t on_host_clipboard) {
    return std::make_unique<win_clipboard_watcher_t>(std::move(on_host_clipboard));
  }

}  // namespace platf

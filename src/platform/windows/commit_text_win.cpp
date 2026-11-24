/**
 * @file src/platform/windows/commit_text_win.cpp
 * @brief Windows SendInput KEYEVENTF_UNICODE backend for atomic UTF-8 COMMIT_TEXT.
 */
#define WINVER 0x0A00

#include <Windows.h>

#include <string>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "src/stream_desk.h"

namespace platf {

  bool commit_text_available() {
    return true;
  }

  bool commit_text(const std::string &utf8) {
    if (utf8.empty() || utf8.size() > LI_STREAM_DESK_MAX_TEXT_BYTES) {
      return false;
    }

    const int wide_len = MultiByteToWideChar(
      CP_UTF8,
      0,
      utf8.data(),
      static_cast<int>(utf8.size()),
      nullptr,
      0
    );
    if (wide_len <= 0) {
      return false;
    }

    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(
          CP_UTF8,
          0,
          utf8.data(),
          static_cast<int>(utf8.size()),
          wide.data(),
          wide_len
        ) != wide_len) {
      return false;
    }

    std::vector<INPUT> events;
    events.reserve(wide.size() * 2);
    for (wchar_t ch : wide) {
      INPUT down {};
      down.type = INPUT_KEYBOARD;
      down.ki.wScan = static_cast<WORD>(ch);
      down.ki.dwFlags = KEYEVENTF_UNICODE;
      events.push_back(down);

      INPUT up {};
      up.type = INPUT_KEYBOARD;
      up.ki.wScan = static_cast<WORD>(ch);
      up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      events.push_back(up);
    }

    const UINT sent = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    return sent == events.size();
  }

}  // namespace platf

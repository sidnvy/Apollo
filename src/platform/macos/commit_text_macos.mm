/**
 * @file src/platform/macos/commit_text_macos.mm
 * @brief macOS CoreGraphics Unicode event backend for atomic UTF-8 COMMIT_TEXT.
 */
#import <ApplicationServices/ApplicationServices.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdint>
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

    CFStringRef cf_str = CFStringCreateWithBytes(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8 *>(utf8.data()),
      static_cast<CFIndex>(utf8.size()),
      kCFStringEncodingUTF8,
      false
    );
    if (cf_str == nullptr) {
      return false;
    }

    const CFIndex total_len = CFStringGetLength(cf_str);
    if (total_len <= 0) {
      CFRelease(cf_str);
      return false;
    }

    std::vector<UniChar> utf16(static_cast<std::size_t>(total_len));
    CFStringGetCharacters(cf_str, CFRangeMake(0, total_len), utf16.data());
    CFRelease(cf_str);

    // CoreGraphics reliably accepts up to 20 UTF-16 code units per Unicode keyboard event.
    constexpr std::size_t kChunkLimit = 20;
    for (std::size_t offset = 0; offset < utf16.size();) {
      std::size_t chunk_len = std::min(kChunkLimit, utf16.size() - offset);
      // Do not split a UTF-16 surrogate pair across two CGEvents.
      if (chunk_len > 1 && offset + chunk_len < utf16.size()) {
        const UniChar last = utf16[offset + chunk_len - 1];
        if (last >= 0xD800 && last <= 0xDBFF) {
          --chunk_len;
        }
      }

      CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
      CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
      if (down == nullptr || up == nullptr) {
        if (down != nullptr) CFRelease(down);
        if (up != nullptr) CFRelease(up);
        return false;
      }

      CGEventKeyboardSetUnicodeString(down, static_cast<UniCharCount>(chunk_len), utf16.data() + offset);
      CGEventKeyboardSetUnicodeString(up, static_cast<UniCharCount>(chunk_len), utf16.data() + offset);
      CGEventPost(kCGHIDEventTap, down);
      CGEventPost(kCGHIDEventTap, up);
      CFRelease(down);
      CFRelease(up);

      offset += chunk_len;
    }

    return true;
  }

}  // namespace platf

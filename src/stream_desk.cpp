/**
 * @file src/stream_desk.cpp
 * @brief Control Channel v1 text-commit reassembly, clipboard deduplication, and stubs.
 */
#include "stream_desk.h"

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/StreamDesk.h>
}

#include "src/logging.h"

namespace stream_desk {

  void text_reassembler_t::reset() {
    has_active_ = false;
    active_msg_id_ = 0;
    buffer_.clear();
  }

  std::optional<std::string> text_reassembler_t::feed(std::string_view payload) {
    SS_COMMIT_TEXT_HEADER hdr {};
    const char *chunk = nullptr;
    if (!LiUnpackCommitTextHeader(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                  static_cast<int>(payload.size()),
                                  &hdr,
                                  &chunk)) {
      return std::nullopt;
    }

    if (!has_active_ || active_msg_id_ != hdr.messageId) {
      reset();
      has_active_ = true;
      active_msg_id_ = hdr.messageId;
    }

    if (buffer_.size() + hdr.payloadLength > LI_STREAM_DESK_MAX_TEXT_BYTES) {
      BOOST_LOG(warning) << "COMMIT_TEXT exceeded max payload limit; dropping messageId=" << hdr.messageId;
      reset();
      return std::nullopt;
    }

    if (hdr.payloadLength > 0 && chunk != nullptr) {
      buffer_.append(chunk, hdr.payloadLength);
    }

    if ((hdr.flags & SS_COMMIT_TEXT_FLAG_FINAL) != 0) {
      std::optional<std::string> result;
      if (!buffer_.empty()) {
        result = std::move(buffer_);
      }
      reset();
      return result;
    }

    return std::nullopt;
  }

  clipboard_reassembler_t::clipboard_reassembler_t(std::uint8_t expected_origin):
      expected_origin_(expected_origin) {
  }

  void clipboard_reassembler_t::reset() {
    has_active_ = false;
    active_event_id_ = 0;
    active_hash_ = 0;
    active_flags_ = 0;
    buffer_.clear();
  }

  std::optional<clipboard_message_t> clipboard_reassembler_t::feed(std::string_view payload) {
    SS_CLIPBOARD_EVENT_HEADER hdr {};
    const char *chunk = nullptr;
    if (!LiUnpackClipboardEventHeader(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                      static_cast<int>(payload.size()),
                                      &hdr,
                                      &chunk)) {
      return std::nullopt;
    }

    if (hdr.origin != expected_origin_) {
      return std::nullopt;
    }

    if (!has_active_ || active_event_id_ != hdr.eventId) {
      reset();
      has_active_ = true;
      active_event_id_ = hdr.eventId;
      active_hash_ = hdr.contentHash;
      active_flags_ = hdr.flags;
    }

    if (buffer_.size() + hdr.payloadLength > LI_STREAM_DESK_MAX_TEXT_BYTES) {
      BOOST_LOG(warning) << "CLIPBOARD_EVENT exceeded max payload limit; dropping eventId=" << hdr.eventId;
      reset();
      return std::nullopt;
    }

    if (hdr.payloadLength > 0 && chunk != nullptr) {
      buffer_.append(chunk, hdr.payloadLength);
    }
    active_flags_ |= hdr.flags;

    if ((hdr.flags & SS_CLIPBOARD_FLAG_FINAL) != 0) {
      std::optional<clipboard_message_t> result;
      if (!buffer_.empty()) {
        clipboard_message_t msg;
        msg.text = std::move(buffer_);
        msg.content_hash = active_hash_;
        msg.origin = expected_origin_;
        msg.flags = active_flags_;
        result = std::move(msg);
      }
      reset();
      return result;
    }

    return std::nullopt;
  }

  bool clipboard_dedup_t::should_forward_host_event(std::uint64_t hash) {
    std::lock_guard<std::mutex> lg(mutex_);
    if (last_from_client_.has_value() && *last_from_client_ == hash) {
      last_from_host_ = hash;
      return false;
    }
    if (last_from_host_.has_value() && *last_from_host_ == hash) {
      return false;
    }
    // Host clipboard changed independently; allow client to re-push that older hash later.
    last_from_client_.reset();
    last_from_host_ = hash;
    return true;
  }

  void clipboard_dedup_t::record_client_push(std::uint64_t hash) {
    std::lock_guard<std::mutex> lg(mutex_);
    last_from_client_ = hash;
    last_from_host_ = hash;
  }

  bool clipboard_dedup_t::should_apply_client_push(std::uint64_t hash) const {
    std::lock_guard<std::mutex> lg(mutex_);
    if (last_from_client_.has_value() && *last_from_client_ == hash) {
      return false;
    }
    return true;
  }

  void clipboard_dedup_t::reset() {
    std::lock_guard<std::mutex> lg(mutex_);
    last_from_client_.reset();
    last_from_host_.reset();
  }

}  // namespace stream_desk

#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)
namespace platf {
  bool commit_text_available() {
    return false;
  }

  bool commit_text(const std::string &) {
    return false;
  }

  bool clipboard_event_available() {
    return false;
  }

  std::unique_ptr<clipboard_watcher_t> create_clipboard_watcher(host_clipboard_cb_t) {
    return nullptr;
  }
}  // namespace platf
#endif

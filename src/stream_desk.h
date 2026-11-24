/**
 * @file src/stream_desk.h
 * @brief Control Channel v1 text-commit and clipboard-sync helpers.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace stream_desk {

  struct clipboard_message_t {
    std::string text;
    std::uint64_t content_hash {0};
    std::uint8_t origin {0};
    std::uint8_t flags {0};
  };

  /**
   * @brief Reassembles chunked SS_COMMIT_TEXT_PTYPE (0x5511) packets.
   */
  class text_reassembler_t {
  public:
    std::optional<std::string> feed(std::string_view payload);
    void reset();

  private:
    bool has_active_ {false};
    std::uint32_t active_msg_id_ {0};
    std::string buffer_;
  };

  /**
   * @brief Reassembles chunked SS_CLIPBOARD_EVENT_PTYPE (0x5512) packets.
   */
  class clipboard_reassembler_t {
  public:
    explicit clipboard_reassembler_t(std::uint8_t expected_origin);

    std::optional<clipboard_message_t> feed(std::string_view payload);
    void reset();

  private:
    std::uint8_t expected_origin_;
    bool has_active_ {false};
    std::uint32_t active_event_id_ {0};
    std::uint64_t active_hash_ {0};
    std::uint8_t active_flags_ {0};
    std::string buffer_;
  };

  /**
   * @brief Tracks content hashes to prevent clipboard echo loops and redundant pushes.
   */
  class clipboard_dedup_t {
  public:
    bool should_forward_host_event(std::uint64_t hash);
    void record_client_push(std::uint64_t hash);
    bool should_apply_client_push(std::uint64_t hash) const;
    void reset();

  private:
    mutable std::mutex mutex_;
    std::optional<std::uint64_t> last_from_client_;
    std::optional<std::uint64_t> last_from_host_;
  };

}  // namespace stream_desk

namespace platf {

  class clipboard_watcher_t {
  public:
    virtual ~clipboard_watcher_t() = default;
    virtual bool set_text(const std::string &utf8, std::uint64_t content_hash) = 0;
  };

  using host_clipboard_cb_t = std::function<void(const std::string &utf8, std::uint64_t content_hash, std::uint8_t flags)>;

  bool commit_text_available();
  bool commit_text(const std::string &utf8);

  bool clipboard_event_available();
  std::unique_ptr<clipboard_watcher_t> create_clipboard_watcher(host_clipboard_cb_t on_host_clipboard);

}  // namespace platf

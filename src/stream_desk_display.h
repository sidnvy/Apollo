/**
 * @file src/stream_desk_display.h
 * @brief Serialized adaptive display state machine and external helper interface.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace stream_desk {

  struct display_caps_t {
    bool adaptive_display {false};
    bool virtual_display {false};
  };

  struct session_display_request_t {
    std::uint32_t session_id {0};
    int width {0};
    int height {0};
    double fps {0.0};
    double desktop_scale {0.0};
    bool virtual_display {false};
  };

  class display_backend_t {
  public:
    virtual ~display_backend_t() = default;
    virtual display_caps_t probe() = 0;
    virtual bool configure(const session_display_request_t &req, std::string &output_name) = 0;
    virtual bool revert() = 0;
  };

  /**
   * @brief Parses desktopScale query argument with legacy scaleFactor fallback.
   */
  double parse_desktop_scale(std::string_view desktop_scale_arg, std::uint32_t legacy_scale_factor);

  /**
   * @brief Normalizes raw launch_session FPS (millihertz or Hz) to Hz.
   */
  double normalize_session_fps(int raw_fps);

  /**
   * @brief Single serialized state machine guarding adaptive display transitions.
   */
  class display_manager_t {
  public:
    enum class state_e {
      IDLE,
      CONFIGURED
    };

    explicit display_manager_t(std::unique_ptr<display_backend_t> backend);

    display_caps_t probe();
    bool configure(const session_display_request_t &req);
    bool revert();

    state_e state() const;
    std::uint64_t generation() const;
    std::string active_output_name() const;

  private:
    static bool is_valid_request(const session_display_request_t &req);

    mutable std::mutex mutex_;
    std::unique_ptr<display_backend_t> backend_;
    state_e state_ {state_e::IDLE};
    std::uint64_t generation_ {0};
    std::string active_output_name_;
  };

  display_manager_t &display_manager();

}  // namespace stream_desk

namespace platf {
  std::unique_ptr<stream_desk::display_backend_t> create_display_helper_backend();
}  // namespace platf

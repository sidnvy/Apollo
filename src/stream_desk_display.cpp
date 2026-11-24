/**
 * @file src/stream_desk_display.cpp
 * @brief Serialized display state machine, query parsers, and non-Linux stubs.
 */
#include "stream_desk_display.h"

#include <cmath>

#include "src/logging.h"

namespace stream_desk {

  double parse_desktop_scale(std::string_view desktop_scale_arg, std::uint32_t legacy_scale_factor) {
    if (!desktop_scale_arg.empty()) {
      bool seen_dot = false;
      bool seen_digit = false;
      double int_part = 0.0;
      double frac_part = 0.0;
      double frac_scale = 0.1;
      bool valid = true;
      for (char ch : desktop_scale_arg) {
        if (ch >= '0' && ch <= '9') {
          seen_digit = true;
          const int digit = ch - '0';
          if (!seen_dot) {
            int_part = (int_part * 10.0) + static_cast<double>(digit);
          } else {
            frac_part += static_cast<double>(digit) * frac_scale;
            frac_scale *= 0.1;
          }
          if (int_part > 10.0) {
            valid = false;
            break;
          }
        } else if (ch == '.' && !seen_dot) {
          seen_dot = true;
        } else {
          valid = false;
          break;
        }
      }
      const double value = int_part + frac_part;
      if (valid && seen_digit && std::isfinite(value)) {
        if (value == 0.0) {
          return 0.0;
        }
        if (value >= 0.5 && value <= 4.0) {
          return value;
        }
      }
      return 0.0;
    }

    if (legacy_scale_factor >= 50 && legacy_scale_factor <= 400 && legacy_scale_factor != 100) {
      return static_cast<double>(legacy_scale_factor) / 100.0;
    }

    return 0.0;
  }

  double normalize_session_fps(int raw_fps) {
    if (raw_fps > 1000) {
      return static_cast<double>(raw_fps) / 1000.0;
    }
    return static_cast<double>(raw_fps);
  }

  display_manager_t::display_manager_t(std::unique_ptr<display_backend_t> backend):
      backend_(std::move(backend)) {
  }

  bool display_manager_t::is_valid_request(const session_display_request_t &req) {
    if (req.width < 320 || req.width > 7680 || req.height < 240 || req.height > 4320) {
      return false;
    }
    if (!std::isfinite(req.fps) || req.fps < 1.0 || req.fps > 480.0) {
      return false;
    }
    if (!std::isfinite(req.desktop_scale)) {
      return false;
    }
    if (req.desktop_scale != 0.0 && (req.desktop_scale < 0.5 || req.desktop_scale > 4.0)) {
      return false;
    }
    return true;
  }

  display_caps_t display_manager_t::probe() {
    std::lock_guard<std::mutex> lg(mutex_);
    if (!backend_) {
      return {};
    }
    return backend_->probe();
  }

  bool display_manager_t::configure(const session_display_request_t &req) {
    std::lock_guard<std::mutex> lg(mutex_);
    if (!backend_ || !is_valid_request(req)) {
      return false;
    }

    std::string output_name;
    if (!backend_->configure(req, output_name)) {
      BOOST_LOG(warning) << "Display helper configure failed for session " << req.session_id;
      return false;
    }

    state_ = state_e::CONFIGURED;
    ++generation_;
    active_output_name_ = std::move(output_name);
    BOOST_LOG(info) << "Configured session display [" << active_output_name_ << "] "
                    << req.width << 'x' << req.height << '@' << req.fps
                    << " scale=" << req.desktop_scale
                    << " virtual=" << req.virtual_display;
    return true;
  }

  bool display_manager_t::revert() {
    std::lock_guard<std::mutex> lg(mutex_);
    if (!backend_) {
      state_ = state_e::IDLE;
      active_output_name_.clear();
      return true;
    }

    const bool ok = backend_->revert();
    state_ = state_e::IDLE;
    ++generation_;
    active_output_name_.clear();
    return ok;
  }

  display_manager_t::state_e display_manager_t::state() const {
    std::lock_guard<std::mutex> lg(mutex_);
    return state_;
  }

  std::uint64_t display_manager_t::generation() const {
    std::lock_guard<std::mutex> lg(mutex_);
    return generation_;
  }

  std::string display_manager_t::active_output_name() const {
    std::lock_guard<std::mutex> lg(mutex_);
    return active_output_name_;
  }

  display_manager_t &display_manager() {
    static display_manager_t instance(platf::create_display_helper_backend());
    return instance;
  }

}  // namespace stream_desk

#if !defined(__linux__) && !defined(__APPLE__)
namespace platf {
  std::unique_ptr<stream_desk::display_backend_t> create_display_helper_backend() {
    return nullptr;
  }
}  // namespace platf
#endif

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace display_device::windows_scale {
  struct status_t {
    std::string output;
    std::string device_path;
    int current_percent {0};
    int minimum_percent {0};
    int maximum_percent {0};
    long error {0};
    bool internal {false};
    bool primary {false};
    unsigned width {0};
    unsigned height {0};
  };

  struct mode_t {
    unsigned width;
    unsigned height;
    unsigned millihertz;
  };

  // Source DPI GET/SET (-3/-4) are runtime-probed Windows extensions. Their
  // relative step indices must never be interpreted as literal percentages.
  std::vector<status_t> inspect();
  bool supported();
  std::string select_output(const std::string &preferred = {}, const std::string &excluded = {});
  std::optional<mode_t> supported_mode(
    const std::string &output,
    unsigned width,
    unsigned height,
    unsigned millihertz
  );
  void initialize(const std::filesystem::path &state_file);
  bool capture_original(const std::string &output);
  bool apply(const std::string &output, double requested_scale, bool remember = true);
  bool restore();
  std::string last_error();
}  // namespace display_device::windows_scale

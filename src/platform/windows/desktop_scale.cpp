#include "desktop_scale.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <thread>

namespace display_device::windows_scale {
  namespace {
    constexpr std::array percentages {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};

    // Windows uses these private packets for per-source desktop scale. Do not
    // infer support from the OS name: validate GET, legal steps, SET and readback.
    struct dpi_get_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      INT32 minimum;
      INT32 current;
      INT32 maximum;
    };

    struct dpi_set_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      INT32 value;
    };

    struct source_t {
      DISPLAYCONFIG_PATH_INFO path;
      std::wstring output;
      std::wstring device_path;
      bool internal;
      bool primary;
    };

    std::mutex state_mutex;
    std::filesystem::path persistence;
    std::map<std::string, int> original_scales;
    std::atomic<bool> known_supported {false};
    thread_local std::string error_text;

    std::string utf8(const std::wstring &value) {
      if (value.empty()) {
        return {};
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }
      std::string out(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
      return out;
    }

    std::vector<source_t> sources() {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      LONG result = ERROR_INSUFFICIENT_BUFFER;
      for (int attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) {
          break;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (result == ERROR_SUCCESS) {
          paths.resize(path_count);
          modes.resize(mode_count);
        }
      }
      if (result != ERROR_SUCCESS) {
        error_text = "QueryDisplayConfig failed: " + std::to_string(result);
        return {};
      }

      std::vector<source_t> out;
      for (const auto &path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
        name.header = {
          DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
          sizeof(name),
          path.sourceInfo.adapterId,
          path.sourceInfo.id,
        };
        DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
        target.header = {
          DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
          sizeof(target),
          path.targetInfo.adapterId,
          path.targetInfo.id,
        };
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
          continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const auto &item) {
              return item.output == name.viewGdiDeviceName;
            })) {
          continue;
        }
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        const bool primary = EnumDisplaySettingsW(name.viewGdiDeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
                             mode.dmPosition.x == 0 && mode.dmPosition.y == 0;
        const auto technology = path.targetInfo.outputTechnology;
        const bool internal = technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                              technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS ||
                              technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                              technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
        out.push_back({path, name.viewGdiDeviceName, target.monitorDevicePath, internal, primary});
      }
      return out;
    }

    LONG query(const source_t &source, dpi_get_t &packet) {
      packet = {};
      packet.header = {
        static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3),
        sizeof(packet),
        source.path.sourceInfo.adapterId,
        source.path.sourceInfo.id,
      };
      const LONG result = DisplayConfigGetDeviceInfo(&packet.header);
      if (result != ERROR_SUCCESS) {
        return result;
      }
      const auto recommended = -static_cast<int64_t>(packet.minimum);
      const auto current = recommended + packet.current;
      if (packet.minimum > 0 || packet.maximum < packet.minimum || recommended < 0 ||
          recommended >= static_cast<int64_t>(percentages.size()) || current < 0 ||
          current >= static_cast<int64_t>(percentages.size())) {
        return ERROR_NOT_SUPPORTED;
      }
      known_supported = true;
      return ERROR_SUCCESS;
    }

    int current_percent(const dpi_get_t &packet) {
      return percentages.at(packet.current - packet.minimum);
    }

    int choose_percent(const dpi_get_t &packet, double requested) {
      const auto maximum = std::min<int64_t>(
        static_cast<int64_t>(packet.maximum) - packet.minimum,
        static_cast<int64_t>(percentages.size()) - 1
      );
      return *std::min_element(
        percentages.begin(),
        percentages.begin() + maximum + 1,
        [requested](int a, int b) {
          return std::abs(a - requested) < std::abs(b - requested);
        }
      );
    }

    bool save() {
      if (persistence.empty()) {
        return true;
      }
      if (original_scales.empty()) {
        std::error_code ec;
        std::filesystem::remove(persistence, ec);
        return !ec;
      }
      std::error_code directory_error;
      if (!persistence.parent_path().empty()) {
        std::filesystem::create_directories(persistence.parent_path(), directory_error);
      }
      if (directory_error) {
        error_text = "Cannot create desktop scale state directory";
        return false;
      }
      auto temporary = persistence;
      temporary += ".tmp";
      std::ofstream stream(temporary, std::ios::trunc);
      stream << "APOLLO-DPI 1\n";
      for (const auto &[id, percent] : original_scales) {
        stream << std::quoted(id) << ' ' << percent << '\n';
      }
      stream.close();
      if (!stream || !MoveFileExW(temporary.c_str(), persistence.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error_text = "Could not persist original desktop scale";
        return false;
      }
      return true;
    }

    bool set(const source_t &source, int percent) {
      dpi_get_t before {};
      LONG result = query(source, before);
      if (result != ERROR_SUCCESS) {
        error_text = "DPI GET unsupported or unavailable: " + std::to_string(result);
        return false;
      }
      if (choose_percent(before, percent) != percent) {
        error_text = "Original DPI is unavailable in the current display mode";
        return false;
      }
      if (current_percent(before) == percent) {
        return true;
      }
      const auto index = static_cast<int>(std::find(percentages.begin(), percentages.end(), percent) - percentages.begin());
      dpi_set_t request {};
      request.header = {
        static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4),
        sizeof(request),
        source.path.sourceInfo.adapterId,
        source.path.sourceInfo.id,
      };
      request.value = index + before.minimum;
      result = DisplayConfigSetDeviceInfo(&request.header);
      if (result != ERROR_SUCCESS) {
        error_text = "DPI SET failed: " + std::to_string(result);
        return false;
      }
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
      do {
        dpi_get_t readback {};
        if (query(source, readback) == ERROR_SUCCESS && current_percent(readback) == percent) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      } while (std::chrono::steady_clock::now() < until);
      error_text = "DPI SET did not match readback";
      return false;
    }
  }  // namespace

  std::vector<status_t> inspect() {
    std::vector<status_t> result;
    for (const auto &source : sources()) {
      status_t status {utf8(source.output), utf8(source.device_path)};
      status.internal = source.internal;
      status.primary = source.primary;
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(source.output.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        status.width = mode.dmPelsWidth;
        status.height = mode.dmPelsHeight;
      }
      dpi_get_t packet {};
      status.error = query(source, packet);
      if (status.error == ERROR_SUCCESS) {
        status.current_percent = current_percent(packet);
        status.minimum_percent = percentages.front();
        status.maximum_percent = choose_percent(packet, percentages.back());
      }
      result.push_back(status);
    }
    return result;
  }

  bool supported() {
    if (known_supported) {
      return true;
    }
    for (const auto &status : inspect()) {
      if (status.error == ERROR_SUCCESS) {
        return true;
      }
    }
    return false;
  }

  std::string select_output(const std::string &preferred, const std::string &excluded) {
    auto active = sources();
    std::erase_if(active, [&](const auto &source) {
      return utf8(source.output) == excluded;
    });
    if (!preferred.empty()) {
      for (const auto &source : active) {
        if (utf8(source.output) == preferred) {
          return preferred;
        }
      }
    }
    for (const auto &source : active) {
      if (source.internal) {
        return utf8(source.output);
      }
    }
    for (const auto &source : active) {
      if (source.primary) {
        return utf8(source.output);
      }
    }
    return active.empty() ? std::string {} : utf8(active.front().output);
  }

  std::optional<mode_t> supported_mode(
    const std::string &output,
    unsigned width,
    unsigned height,
    unsigned millihertz
  ) {
    if (!width || !height) {
      return std::nullopt;
    }
    for (const auto &source : sources()) {
      if (utf8(source.output) != output) {
        continue;
      }
      std::optional<mode_t> best;
      double best_score = INFINITY;
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      for (DWORD index = 0; EnumDisplaySettingsW(source.output.c_str(), index, &mode); ++index) {
        if (!mode.dmPelsWidth || !mode.dmPelsHeight || mode.dmBitsPerPel < 24 || (mode.dmDisplayFlags & DM_INTERLACED)) {
          continue;
        }
        const double aspect = (static_cast<double>(mode.dmPelsWidth) / mode.dmPelsHeight) / (static_cast<double>(width) / height);
        const double area = (static_cast<double>(mode.dmPelsWidth) * mode.dmPelsHeight) / (static_cast<double>(width) * height);
        const double score = 4 * std::abs(std::log(aspect)) + std::abs(std::log(area)) +
                             std::abs(static_cast<double>(mode.dmDisplayFrequency) * 1000 - millihertz) / 1000000.0;
        if (score < best_score) {
          best_score = score;
          best = mode_t {mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency * 1000};
        }
      }
      return best;
    }
    return std::nullopt;
  }

  void initialize(const std::filesystem::path &path) {
    std::lock_guard lock(state_mutex);
    persistence = path;
    original_scales.clear();
    std::ifstream input(path);
    std::string marker;
    int version = 0;
    input >> marker >> version;
    if (marker != "APOLLO-DPI" || version != 1) {
      return;
    }
    std::string id;
    int percentage = 0;
    while (input >> std::quoted(id) >> percentage) {
      if (std::find(percentages.begin(), percentages.end(), percentage) != percentages.end()) {
        original_scales.emplace(id, percentage);
      }
    }
  }

  bool capture_original(const std::string &output) {
    std::lock_guard lock(state_mutex);
    for (const auto &source : sources()) {
      if (utf8(source.output) != output) {
        continue;
      }
      dpi_get_t before {};
      const auto code = query(source, before);
      if (code != ERROR_SUCCESS) {
        error_text = "DPI GET unsupported or unavailable: " + std::to_string(code);
        return false;
      }
      const auto id = utf8(source.device_path);
      if (id.empty()) {
        error_text = "Display has no stable device path";
        return false;
      }
      if (original_scales.contains(id)) {
        return true;
      }
      original_scales.emplace(id, current_percent(before));
      if (!save()) {
        original_scales.erase(id);
        return false;
      }
      return true;
    }
    error_text = "Requested display is no longer active";
    return false;
  }

  bool apply(const std::string &output, double requested_scale, bool remember) {
    if (requested_scale == 0) {
      return true;
    }
    if (!std::isfinite(requested_scale) || requested_scale <= 0) {
      return false;
    }
    std::lock_guard lock(state_mutex);
    for (const auto &source : sources()) {
      if (utf8(source.output) != output) {
        continue;
      }
      dpi_get_t before {};
      const auto code = query(source, before);
      if (code != ERROR_SUCCESS) {
        error_text = "DPI GET unsupported or unavailable: " + std::to_string(code);
        return false;
      }
      const auto target = choose_percent(before, requested_scale * 100);
      const auto id = utf8(source.device_path);
      if (remember && !id.empty() && !original_scales.contains(id)) {
        original_scales.emplace(id, current_percent(before));
        if (!save()) {
          original_scales.erase(id);
          return false;
        }
      }
      return set(source, target);
    }
    error_text = "Requested display is no longer active";
    return false;
  }

  bool restore() {
    std::lock_guard lock(state_mutex);
    const auto active = sources();
    bool ok = true;
    for (auto it = original_scales.begin(); it != original_scales.end();) {
      auto source = std::find_if(active.begin(), active.end(), [&](const auto &item) {
        return utf8(item.device_path) == it->first;
      });
      if (source == active.end() || !set(*source, it->second)) {
        ok = false;
        ++it;
      } else {
        it = original_scales.erase(it);
      }
    }
    return save() && ok;
  }

  std::string last_error() {
    return error_text;
  }
}  // namespace display_device::windows_scale

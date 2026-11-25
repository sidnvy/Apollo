/**
 * @file src/platform/macos/input_geometry.h
 * @brief Shared capture target for macOS pointer events.
 */
#pragma once

#include <atomic>
#include <CoreGraphics/CoreGraphics.h>

namespace platf::macos {
  inline std::atomic<CGDirectDisplayID> captured_display {kCGNullDirectDisplay};

  /**
   * @brief Returns the currently captured display ID when active, else the main display.
   */
  inline CGDirectDisplayID input_display() {
    const auto display = captured_display.load(std::memory_order_relaxed);
    return display != kCGNullDirectDisplay && CGDisplayIsActive(display)
             ? display
             : CGMainDisplayID();
  }
}  // namespace platf::macos

/**
 * @file tests/unit/test_stream_desk_display.cpp
 * @brief Unit tests for M3 serialized display manager, scale/fps parsers, and Linux JSON helper runner.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "src/stream_desk_display.h"

namespace {

  class fake_display_backend_t final: public stream_desk::display_backend_t {
  public:
    stream_desk::display_caps_t probe() override {
      return {true, true};
    }

    bool configure(const stream_desk::session_display_request_t &req, std::string &output_name) override {
      ++configure_calls;
      last_request = req;
      output_name = req.virtual_display ? "HEADLESS-2" : "eDP-1";
      return true;
    }

    bool revert() override {
      ++revert_calls;
      return true;
    }

    int configure_calls {0};
    int revert_calls {0};
    stream_desk::session_display_request_t last_request {};
  };

}  // namespace

TEST(StreamDeskDisplayTest, ParseDesktopScaleValidAndFallback) {
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("", 100), 0.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("", 150), 1.5);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("0", 150), 0.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("1.75", 100), 1.75);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("0.5", 100), 0.5);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("4.0", 100), 4.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("0.49", 100), 0.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("4.1", 100), 0.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("nan", 100), 0.0);
  EXPECT_DOUBLE_EQ(stream_desk::parse_desktop_scale("1.5abc", 100), 0.0);
}

TEST(StreamDeskDisplayTest, NormalizeSessionFpsMillihertzAndHz) {
  EXPECT_DOUBLE_EQ(stream_desk::normalize_session_fps(60000), 60.0);
  EXPECT_DOUBLE_EQ(stream_desk::normalize_session_fps(119880), 119.88);
  EXPECT_DOUBLE_EQ(stream_desk::normalize_session_fps(120), 120.0);
  EXPECT_DOUBLE_EQ(stream_desk::normalize_session_fps(60), 60.0);
}

TEST(StreamDeskDisplayTest, StateMachineSerializesConfigureAndIdempotentRevert) {
  auto backend_ptr = std::make_unique<fake_display_backend_t>();
  auto *raw_backend = backend_ptr.get();
  stream_desk::display_manager_t mgr(std::move(backend_ptr));

  EXPECT_EQ(mgr.state(), stream_desk::display_manager_t::state_e::IDLE);
  EXPECT_EQ(mgr.generation(), 0ULL);

  // Reject out-of-bounds width / scale without calling backend
  EXPECT_FALSE(mgr.configure({1, 100, 1080, 60.0, 1.0, false}));
  EXPECT_FALSE(mgr.configure({2, 1920, 1080, 60.0, 9.0, false}));
  EXPECT_EQ(raw_backend->configure_calls, 0);

  EXPECT_TRUE(mgr.configure({10, 2184, 1968, 120.0, 1.5, true}));
  EXPECT_EQ(mgr.state(), stream_desk::display_manager_t::state_e::CONFIGURED);
  EXPECT_EQ(mgr.generation(), 1ULL);
  EXPECT_EQ(mgr.active_output_name(), "HEADLESS-2");

  EXPECT_TRUE(mgr.revert());
  EXPECT_EQ(mgr.state(), stream_desk::display_manager_t::state_e::IDLE);
  EXPECT_EQ(mgr.generation(), 2ULL);
  EXPECT_TRUE(mgr.active_output_name().empty());

  // Idempotent repeat revert
  EXPECT_TRUE(mgr.revert());
  EXPECT_EQ(mgr.state(), stream_desk::display_manager_t::state_e::IDLE);
  EXPECT_EQ(raw_backend->revert_calls, 2);
}

#ifdef __linux__
TEST(StreamDeskDisplayTest, LinuxHelperRunnerProbeConfigureRevert) {
  const std::string helper_path = "/tmp/apollo-test-helper-" + std::to_string(::getpid()) + ".py";
  const std::string log_path = "/tmp/apollo-test-helper-log-" + std::to_string(::getpid()) + ".txt";
  {
    std::ofstream fh(helper_path);
    fh << "#!/usr/bin/env python3\n"
       << "import json, sys\n"
       << "req = json.loads(sys.stdin.read())\n"
       << "with open('" << log_path << "', 'a') as lf:\n"
       << "    lf.write(json.dumps(req, sort_keys=True) + '\\n')\n"
       << "act = req.get('action')\n"
       << "if act == 'probe':\n"
       << "    print(json.dumps({'ok': True, 'adaptiveDisplay': True, 'virtualDisplay': True}))\n"
       << "elif act == 'configure':\n"
       << "    print(json.dumps({'ok': True, 'outputName': 'HEADLESS-9'}))\n"
       << "elif act == 'revert':\n"
       << "    print(json.dumps({'ok': True}))\n";
  }
  ::chmod(helper_path.c_str(), 0755);
  std::filesystem::remove(log_path);

  setenv("STREAM_DESK_DISPLAY_HELPER", helper_path.c_str(), 1);
  stream_desk::display_manager_t mgr(platf::create_display_helper_backend());

  auto caps = mgr.probe();
  EXPECT_TRUE(caps.adaptive_display);
  EXPECT_TRUE(caps.virtual_display);

  EXPECT_TRUE(mgr.configure({7, 2560, 1440, 120.0, 1.25, true}));
  EXPECT_EQ(mgr.active_output_name(), "HEADLESS-9");
  EXPECT_TRUE(mgr.revert());
  EXPECT_TRUE(mgr.active_output_name().empty());

  unsetenv("STREAM_DESK_DISPLAY_HELPER");
  std::filesystem::remove(helper_path);
  std::filesystem::remove(log_path);
}

TEST(StreamDeskDisplayTest, LinuxHelperRunnerTimeoutKillsAndReapsChild) {
  const std::string helper_path = "/tmp/apollo-test-hang-helper-" + std::to_string(::getpid()) + ".sh";
  {
    std::ofstream fh(helper_path);
    fh << "#!/bin/sh\nsleep 10\n";
  }
  ::chmod(helper_path.c_str(), 0755);

  setenv("STREAM_DESK_DISPLAY_HELPER", helper_path.c_str(), 1);
  auto backend = platf::create_display_helper_backend();
  auto caps = backend->probe();
  EXPECT_FALSE(caps.adaptive_display);
  EXPECT_FALSE(caps.virtual_display);

  // Confirm no unreaped child remains for this process
  int status = 0;
  pid_t waited = ::waitpid(-1, &status, WNOHANG);
  EXPECT_TRUE(waited == -1 && errno == ECHILD);

  unsetenv("STREAM_DESK_DISPLAY_HELPER");
  std::filesystem::remove(helper_path);
}
#endif

#ifdef _WIN32
  #include "src/platform/windows/desktop_scale.h"

TEST(StreamDeskDisplayTest, WindowsScaleInspectReportsValidMonitors) {
  const auto statuses = display_device::windows_scale::inspect();
  for (const auto &status : statuses) {
    EXPECT_FALSE(status.output.empty());
    if (status.error == 0) {
      EXPECT_GE(status.current_percent, 100);
      EXPECT_LE(status.current_percent, 500);
      EXPECT_GE(status.minimum_percent, 100);
      EXPECT_GE(status.maximum_percent, status.current_percent);
    }
  }
}
#endif

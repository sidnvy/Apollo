/**
 * @file src/platform/linux/mic_pulse.cpp
 * @brief Linux PipeWire/PulseAudio virtual sink writer for reverse microphone audio.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

#include "src/logging.h"
#include "src/mic.h"
#include "src/platform/common.h"

namespace {

  std::string target_sink_name() {
    const char *env = std::getenv("MOONLIGHT_MIC_SINK");
    if (env != nullptr && env[0] != '\0') {
      return std::string(env);
    }
    return "moonlight_mic_sink";
  }

  struct sink_probe_state_t {
    pa_mainloop *ml {nullptr};
    std::string target_name;
    bool done {false};
    bool ok {false};
  };

  void on_sink_info(pa_context *, const pa_sink_info *info, int eol, void *userdata) {
    auto *state = static_cast<sink_probe_state_t *>(userdata);
    if (eol < 0) {
      state->ok = false;
      state->done = true;
      pa_mainloop_quit(state->ml, 1);
      return;
    }
    if (eol > 0) {
      state->done = true;
      pa_mainloop_quit(state->ml, 0);
      return;
    }
    if (info != nullptr && info->monitor_source_name != nullptr && info->monitor_source_name[0] != '\0') {
      state->ok = true;
    }
  }

  void on_context_state(pa_context *ctx, void *userdata) {
    auto *state = static_cast<sink_probe_state_t *>(userdata);
    switch (pa_context_get_state(ctx)) {
      case PA_CONTEXT_READY:
        {
          pa_operation *op = pa_context_get_sink_info_by_name(ctx, state->target_name.c_str(), on_sink_info, state);
          if (op == nullptr) {
            state->ok = false;
            state->done = true;
            pa_mainloop_quit(state->ml, 1);
          } else {
            pa_operation_unref(op);
          }
          break;
        }
      case PA_CONTEXT_FAILED:
      case PA_CONTEXT_TERMINATED:
        if (!state->done) {
          state->ok = false;
          state->done = true;
        }
        pa_mainloop_quit(state->ml, 1);
        break;
      default:
        break;
    }
  }

  class pulse_mic_sink_t final: public mic::sink_t {
  public:
    pulse_mic_sink_t(pa_simple *stream, std::string sink_name):
        stream_(stream),
        sink_name_(std::move(sink_name)),
        worker_(&pulse_mic_sink_t::run, this) {
    }

    ~pulse_mic_sink_t() override {
      close();
    }

    bool write_pcm(const int16_t *samples, std::size_t count) override {
      if (samples == nullptr || count == 0) {
        return true;
      }

      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        return false;
      }

      constexpr std::size_t max_queued_frames = 6;  // 120 ms cap
      if (queue_.size() >= max_queued_frames) {
        queue_.pop_front();
      }
      queue_.emplace_back(samples, samples + count);
      cv_.notify_one();
      return true;
    }

    void close() override {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
          return;
        }
        closed_ = true;
        stop_ = true;
      }
      cv_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
      if (stream_ != nullptr) {
        pa_simple_free(stream_);
        stream_ = nullptr;
      }
    }

  private:
    void run() {
      for (;;) {
        std::vector<int16_t> frame;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait(lock, [this] {
            return stop_ || !queue_.empty();
          });
          if (stop_) {
            break;
          }
          frame = std::move(queue_.front());
          queue_.pop_front();
        }

        int err = 0;
        if (pa_simple_write(stream_, frame.data(), frame.size() * sizeof(int16_t), &err) < 0) {
          BOOST_LOG(warning) << "Reverse mic pa_simple_write failed on [" << sink_name_
                             << "]: " << pa_strerror(err);
          std::lock_guard<std::mutex> lock(mutex_);
          stop_ = true;
          break;
        }
      }
    }

    pa_simple *stream_ {nullptr};
    std::string sink_name_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> queue_;
    bool stop_ {false};
    bool closed_ {false};
    std::thread worker_;
  };

}  // namespace

namespace platf {

  bool mic_available() {
    sink_probe_state_t state;
    state.target_name = target_sink_name();
    state.ml = pa_mainloop_new();
    if (state.ml == nullptr) {
      return false;
    }

    pa_mainloop_api *api = pa_mainloop_get_api(state.ml);
    pa_context *ctx = pa_context_new(api, "ApolloMicProbe");
    if (ctx == nullptr) {
      pa_mainloop_free(state.ml);
      return false;
    }

    pa_context_set_state_callback(ctx, on_context_state, &state);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
      pa_context_unref(ctx);
      pa_mainloop_free(state.ml);
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (!state.done) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
      const int wait_us = static_cast<int>(std::min<long long>(remaining_us, 25000));
      if (pa_mainloop_prepare(state.ml, wait_us) < 0) {
        break;
      }
      if (pa_mainloop_poll(state.ml) < 0) {
        break;
      }
      if (pa_mainloop_dispatch(state.ml) < 0) {
        break;
      }
    }

    const bool available = state.ok;
    pa_context_set_state_callback(ctx, nullptr, nullptr);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(state.ml);
    return available;
  }

  std::unique_ptr<mic::sink_t> create_mic_sink() {
    const std::string sink_name = target_sink_name();
    if (!mic_available()) {
      BOOST_LOG(warning) << "Reverse mic sink [" << sink_name << "] is unavailable; refusing open";
      return nullptr;
    }

    static constexpr pa_sample_spec ss {
      .format = PA_SAMPLE_S16LE,
      .rate = LI_MIC_SAMPLE_RATE,
      .channels = LI_MIC_CHANNELS,
    };

    const uint32_t frame_bytes = static_cast<uint32_t>(LI_MIC_FRAME_SAMPLES * sizeof(int16_t));
    const pa_buffer_attr attr {
      .maxlength = frame_bytes * 8,
      .tlength = frame_bytes * 4,
      .prebuf = static_cast<uint32_t>(-1),
      .minreq = frame_bytes,
      .fragsize = static_cast<uint32_t>(-1),
    };

    int err = 0;
    pa_simple *stream = pa_simple_new(
      nullptr,
      "Apollo",
      PA_STREAM_PLAYBACK,
      sink_name.c_str(),
      "Apollo Reverse Mic",
      &ss,
      nullptr,
      &attr,
      &err
    );
    if (stream == nullptr) {
      BOOST_LOG(warning) << "Failed to open reverse mic stream on [" << sink_name
                         << "]: " << pa_strerror(err);
      return nullptr;
    }

    BOOST_LOG(info) << "Reverse mic stream opened on sink [" << sink_name << "]";
    return std::make_unique<pulse_mic_sink_t>(stream, sink_name);
  }

}  // namespace platf

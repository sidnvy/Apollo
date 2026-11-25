/**
 * @file src/platform/macos/mic_coreaudio.mm
 * @brief macOS CoreAudio HAL AudioUnit reverse microphone sink targeting BlackHole.
 */
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Mic.h>
}

#include "src/logging.h"
#include "src/mic.h"
#include "src/platform/common.h"

namespace {

  constexpr std::size_t kMaxBufferedSamples = LI_MIC_SAMPLE_RATE / 5;  // 200ms @ 48kHz mono
  constexpr UInt32 kOutputChannels = 2;

  bool icontains(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) {
      return false;
    }
    auto it = std::search(
      haystack.begin(),
      haystack.end(),
      needle.begin(),
      needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      }
    );
    return it != haystack.end();
  }

  std::string cfstring_to_utf8(CFStringRef str) {
    if (str == nullptr) {
      return {};
    }
    if (const char *fast = CFStringGetCStringPtr(str, kCFStringEncodingUTF8)) {
      return std::string(fast);
    }
    const CFIndex len = CFStringGetLength(str);
    const CFIndex max_size = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max_size), '\0');
    if (!CFStringGetCString(str, out.data(), max_size, kCFStringEncodingUTF8)) {
      return {};
    }
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
  }

  void detach_coreaudio_runloop() {
    static std::once_flag once;
    std::call_once(once, []() {
      CFRunLoopRef null_runloop = nullptr;
      AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyRunLoop,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      AudioObjectSetPropertyData(
        kAudioObjectSystemObject,
        &addr,
        0,
        nullptr,
        sizeof(null_runloop),
        &null_runloop
      );
    });
  }

  bool device_has_scope_channels(AudioDeviceID device_id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr {
      kAudioDevicePropertyStreamConfiguration,
      scope,
      kAudioObjectPropertyElementMain
    };
    UInt32 data_size = 0;
    if (AudioObjectGetPropertyDataSize(device_id, &addr, 0, nullptr, &data_size) != noErr || data_size == 0) {
      return false;
    }

    std::vector<std::uint8_t> storage(data_size);
    auto *buf_list = reinterpret_cast<AudioBufferList *>(storage.data());
    if (AudioObjectGetPropertyData(device_id, &addr, 0, nullptr, &data_size, buf_list) != noErr) {
      return false;
    }

    for (UInt32 i = 0; i < buf_list->mNumberBuffers; ++i) {
      if (buf_list->mBuffers[i].mNumberChannels > 0) {
        return true;
      }
    }
    return false;
  }

  std::string get_device_name(AudioDeviceID dev) {
    if (dev == kAudioObjectUnknown) {
      return {};
    }
    AudioObjectPropertyAddress name_addr {
      kAudioDevicePropertyDeviceNameCFString,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    CFStringRef cf_name = nullptr;
    UInt32 name_size = sizeof(cf_name);
    if (AudioObjectGetPropertyData(dev, &name_addr, 0, nullptr, &name_size, &cf_name) != noErr || cf_name == nullptr) {
      return {};
    }
    const std::string name = cfstring_to_utf8(cf_name);
    CFRelease(cf_name);
    return name;
  }

  bool get_default_input_device(AudioDeviceID &out_id) {
    AudioObjectPropertyAddress addr {
      kAudioHardwarePropertyDefaultInputDevice,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &dev) != noErr ||
        dev == kAudioObjectUnknown) {
      return false;
    }
    out_id = dev;
    return true;
  }

  OSStatus set_default_input_device(AudioDeviceID dev_id) {
    AudioObjectPropertyAddress addr {
      kAudioHardwarePropertyDefaultInputDevice,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, sizeof(dev_id), &dev_id);
  }

  bool device_is_alive(AudioDeviceID dev_id) {
    if (dev_id == kAudioObjectUnknown) {
      return false;
    }
    AudioObjectPropertyAddress addr {
      kAudioDevicePropertyDeviceIsAlive,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    if (AudioObjectGetPropertyData(dev_id, &addr, 0, nullptr, &size, &alive) != noErr || alive == 0) {
      return false;
    }
    return device_has_scope_channels(dev_id, kAudioDevicePropertyScopeInput);
  }

  bool find_virtual_mic_device(AudioDeviceID &out_id, std::string &out_name) {
    detach_coreaudio_runloop();
    AudioObjectPropertyAddress devices_addr {
      kAudioHardwarePropertyDevices,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };

    UInt32 data_size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices_addr, 0, nullptr, &data_size) != noErr ||
        data_size < sizeof(AudioDeviceID)) {
      return false;
    }

    std::vector<AudioDeviceID> devices(data_size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_addr, 0, nullptr, &data_size, devices.data()) != noErr) {
      return false;
    }

    std::string target_needle = "BlackHole";
    if (const char *env = std::getenv("STREAM_DESK_MIC_DEVICE"); env != nullptr && env[0] != '\0') {
      target_needle = env;
    }

    for (AudioDeviceID dev : devices) {
      // A loopback virtual microphone must expose both render (output) and capture (input) streams.
      if (!device_has_scope_channels(dev, kAudioDevicePropertyScopeOutput) ||
          !device_has_scope_channels(dev, kAudioDevicePropertyScopeInput)) {
        continue;
      }

      const std::string name = get_device_name(dev);
      if (icontains(name, target_needle)) {
        out_id = dev;
        out_name = name;
        return true;
      }
    }

    return false;
  }

  class coreaudio_mic_sink_t final : public mic::sink_t {
  public:
    coreaudio_mic_sink_t(AudioDeviceID device_id, std::string device_name):
        device_id_(device_id),
        device_name_(std::move(device_name)) {
      open_unit();
    }

    ~coreaudio_mic_sink_t() override {
      close();
    }

    bool is_open() const {
      std::lock_guard<std::mutex> lg(unit_mutex_);
      return !closed_ && unit_ != nullptr;
    }

    bool write_pcm(const int16_t *samples, std::size_t count) override {
      if (samples == nullptr || count == 0) {
        return true;
      }
      {
        std::lock_guard<std::mutex> ul(unit_mutex_);
        if (closed_ || unit_ == nullptr) {
          return false;
        }
      }

      std::lock_guard<std::mutex> bl(buf_mutex_);
      for (std::size_t i = 0; i < count; ++i) {
        if (pcm_queue_.size() >= kMaxBufferedSamples) {
          pcm_queue_.pop_front();
        }
        pcm_queue_.push_back(samples[i]);
      }
      return true;
    }

    void close() override {
      AudioComponentInstance unit_to_dispose = nullptr;
      bool was_started = false;
      bool restore_needed = false;
      AudioDeviceID restore_id = kAudioObjectUnknown;
      std::string restore_name;
      {
        std::lock_guard<std::mutex> ul(unit_mutex_);
        if (closed_) {
          return;
        }
        closed_ = true;
        unit_to_dispose = unit_;
        unit_ = nullptr;
        was_started = started_;
        started_ = false;
        restore_needed = switched_default_input_;
        switched_default_input_ = false;
        restore_id = previous_default_input_;
        previous_default_input_ = kAudioObjectUnknown;
        restore_name = std::move(previous_default_input_name_);
        previous_default_input_name_.clear();
      }

      if (unit_to_dispose != nullptr) {
        if (was_started) {
          AudioOutputUnitStop(unit_to_dispose);
        }
        AudioUnitUninitialize(unit_to_dispose);
        AudioComponentInstanceDispose(unit_to_dispose);
      }

      {
        std::lock_guard<std::mutex> bl(buf_mutex_);
        pcm_queue_.clear();
      }

      if (restore_needed) {
        AudioDeviceID cur_id = kAudioObjectUnknown;
        if (get_default_input_device(cur_id) && cur_id == device_id_ && device_is_alive(restore_id)) {
          const OSStatus status = set_default_input_device(restore_id);
          if (status == noErr) {
            BOOST_LOG(info) << "Restored macOS default input device to [" << restore_name << "]";
          } else {
            BOOST_LOG(warning) << "CoreAudio reverse mic failed restoring default input device [" << restore_name << "]: " << status;
          }
        }
      }
    }

  private:
    static OSStatus render_callback(
      void *inRefCon,
      AudioUnitRenderActionFlags *,
      const AudioTimeStamp *,
      UInt32,
      UInt32 inNumberFrames,
      AudioBufferList *ioData
    ) {
      auto *self = static_cast<coreaudio_mic_sink_t *>(inRefCon);
      if (ioData == nullptr || ioData->mNumberBuffers == 0) {
        return noErr;
      }

      auto *dst = static_cast<int16_t *>(ioData->mBuffers[0].mData);
      if (dst == nullptr) {
        return noErr;
      }

      std::lock_guard<std::mutex> bl(self->buf_mutex_);
      const std::size_t available = self->pcm_queue_.size();
      const std::size_t to_copy = std::min<std::size_t>(inNumberFrames, available);
      for (std::size_t i = 0; i < to_copy; ++i) {
        const int16_t sample = self->pcm_queue_.front();
        self->pcm_queue_.pop_front();
        dst[2 * i] = sample;
        dst[2 * i + 1] = sample;
      }
      if (to_copy < inNumberFrames) {
        std::memset(
          dst + to_copy * kOutputChannels,
          0,
          (inNumberFrames - to_copy) * kOutputChannels * sizeof(int16_t)
        );
      }
      ioData->mBuffers[0].mDataByteSize = inNumberFrames * kOutputChannels * sizeof(int16_t);
      return noErr;
    }

    void open_unit() {
      detach_coreaudio_runloop();
      AudioComponentDescription desc {};
      desc.componentType = kAudioUnitType_Output;
      desc.componentSubType = kAudioUnitSubType_HALOutput;
      desc.componentManufacturer = kAudioUnitManufacturer_Apple;

      AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
      if (comp == nullptr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic failed to find HALOutput component";
        close();
        return;
      }

      OSStatus status = AudioComponentInstanceNew(comp, &unit_);
      if (status != noErr || unit_ == nullptr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic AudioComponentInstanceNew failed: " << status;
        close();
        return;
      }

      status = AudioUnitSetProperty(
        unit_,
        kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global,
        0,
        &device_id_,
        sizeof(device_id_)
      );
      if (status != noErr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic failed binding device [" << device_name_ << "]: " << status;
        close();
        return;
      }

      AudioStreamBasicDescription format {};
      format.mSampleRate = static_cast<Float64>(LI_MIC_SAMPLE_RATE);
      format.mFormatID = kAudioFormatLinearPCM;
      format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
      format.mFramesPerPacket = 1;
      format.mChannelsPerFrame = kOutputChannels;
      format.mBitsPerChannel = 16;
      format.mBytesPerFrame = kOutputChannels * sizeof(int16_t);
      format.mBytesPerPacket = format.mBytesPerFrame;

      status = AudioUnitSetProperty(
        unit_,
        kAudioUnitProperty_StreamFormat,
        kAudioUnitScope_Input,
        0,
        &format,
        sizeof(format)
      );
      if (status != noErr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic failed setting stream format: " << status;
        close();
        return;
      }

      AURenderCallbackStruct cb {};
      cb.inputProc = &coreaudio_mic_sink_t::render_callback;
      cb.inputProcRefCon = this;
      status = AudioUnitSetProperty(
        unit_,
        kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input,
        0,
        &cb,
        sizeof(cb)
      );
      if (status != noErr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic failed setting render callback: " << status;
        close();
        return;
      }

      status = AudioUnitInitialize(unit_);
      if (status != noErr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic AudioUnitInitialize failed: " << status;
        close();
        return;
      }

      status = AudioOutputUnitStart(unit_);
      if (status != noErr) {
        BOOST_LOG(warning) << "CoreAudio reverse mic AudioOutputUnitStart failed: " << status;
        close();
        return;
      }

      started_ = true;
      BOOST_LOG(info) << "CoreAudio reverse mic sink active on [" << device_name_ << "]";

      AudioDeviceID prev_id = kAudioObjectUnknown;
      if (get_default_input_device(prev_id) && prev_id != device_id_) {
        std::string prev_name = get_device_name(prev_id);
        const OSStatus switch_status = set_default_input_device(device_id_);
        if (switch_status == noErr) {
          previous_default_input_ = prev_id;
          previous_default_input_name_ = std::move(prev_name);
          switched_default_input_ = true;
          BOOST_LOG(info) << "Switched macOS default input device from [" << previous_default_input_name_ << "] to [" << device_name_ << "]";
        } else {
          BOOST_LOG(warning) << "CoreAudio reverse mic could not switch default input device to [" << device_name_ << "]: " << switch_status;
        }
      }
    }

    AudioDeviceID device_id_ {0};
    std::string device_name_;
    mutable std::mutex unit_mutex_;
    std::mutex buf_mutex_;
    AudioComponentInstance unit_ {nullptr};
    std::deque<int16_t> pcm_queue_;
    bool started_ {false};
    bool closed_ {false};
    bool switched_default_input_ {false};
    AudioDeviceID previous_default_input_ {kAudioObjectUnknown};
    std::string previous_default_input_name_;
  };

}  // namespace

namespace platf {

  bool mic_available() {
    static std::mutex cache_mutex;
    static bool has_cached = false;
    static bool cached_value = false;
    static std::chrono::steady_clock::time_point last_checked {};

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (has_cached && (now - last_checked) < std::chrono::seconds(5)) {
      return cached_value;
    }

    AudioDeviceID id = 0;
    std::string name;
    cached_value = find_virtual_mic_device(id, name);
    last_checked = std::chrono::steady_clock::now();
    has_cached = true;
    return cached_value;
  }

  std::unique_ptr<mic::sink_t> create_mic_sink() {
    AudioDeviceID id = 0;
    std::string name;
    if (!find_virtual_mic_device(id, name)) {
      BOOST_LOG(warning) << "macOS virtual mic device (BlackHole) unavailable; refusing open";
      return nullptr;
    }

    auto sink = std::make_unique<coreaudio_mic_sink_t>(id, std::move(name));
    if (!sink->is_open()) {
      return nullptr;
    }
    return sink;
  }

}  // namespace platf

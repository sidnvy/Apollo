/**
 * @file src/platform/macos/mic_coreaudio.mm
 * @brief macOS CoreAudio HAL AudioUnit reverse microphone sink targeting BlackHole.
 */
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cctype>
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

  bool device_has_output_channels(AudioDeviceID device_id) {
    AudioObjectPropertyAddress addr {
      kAudioDevicePropertyStreamConfiguration,
      kAudioDevicePropertyScopeOutput,
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

  bool find_virtual_mic_output_device(AudioDeviceID &out_id, std::string &out_name) {
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
      if (!device_has_output_channels(dev)) {
        continue;
      }

      AudioObjectPropertyAddress name_addr {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      CFStringRef cf_name = nullptr;
      UInt32 name_size = sizeof(cf_name);
      if (AudioObjectGetPropertyData(dev, &name_addr, 0, nullptr, &name_size, &cf_name) != noErr || cf_name == nullptr) {
        continue;
      }

      const std::string name = cfstring_to_utf8(cf_name);
      CFRelease(cf_name);

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
      {
        std::lock_guard<std::mutex> ul(unit_mutex_);
        if (closed_) {
          return;
        }
        closed_ = true;
        unit_to_dispose = unit_;
        unit_ = nullptr;
      }

      if (unit_to_dispose != nullptr) {
        if (started_) {
          AudioOutputUnitStop(unit_to_dispose);
          started_ = false;
        }
        AudioUnitUninitialize(unit_to_dispose);
        AudioComponentInstanceDispose(unit_to_dispose);
      }

      std::lock_guard<std::mutex> bl(buf_mutex_);
      pcm_queue_.clear();
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
        dst[i] = self->pcm_queue_.front();
        self->pcm_queue_.pop_front();
      }
      if (to_copy < inNumberFrames) {
        std::memset(dst + to_copy, 0, (inNumberFrames - to_copy) * sizeof(int16_t));
      }
      ioData->mBuffers[0].mDataByteSize = inNumberFrames * sizeof(int16_t);
      return noErr;
    }

    void open_unit() {
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
      format.mChannelsPerFrame = LI_MIC_CHANNELS;
      format.mBitsPerChannel = 16;
      format.mBytesPerFrame = format.mChannelsPerFrame * sizeof(int16_t);
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
    }

    AudioDeviceID device_id_ {0};
    std::string device_name_;
    mutable std::mutex unit_mutex_;
    std::mutex buf_mutex_;
    AudioComponentInstance unit_ {nullptr};
    std::deque<int16_t> pcm_queue_;
    bool started_ {false};
    bool closed_ {false};
  };

}  // namespace

namespace platf {

  bool mic_available() {
    AudioDeviceID id = 0;
    std::string name;
    return find_virtual_mic_output_device(id, name);
  }

  std::unique_ptr<mic::sink_t> create_mic_sink() {
    AudioDeviceID id = 0;
    std::string name;
    if (!find_virtual_mic_output_device(id, name)) {
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

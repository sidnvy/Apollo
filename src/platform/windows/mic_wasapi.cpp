/**
 * @file src/platform/windows/mic_wasapi.cpp
 * @brief Windows WASAPI reverse microphone sink targeting a virtual mic render endpoint.
 */
#define WINVER 0x0A00

#include <Windows.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

  // File-local key avoids INITGUID duplicate-definition conflicts with audio.cpp.
  const PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
  };

  std::string wide_to_utf8(const wchar_t *src) {
    if (src == nullptr || src[0] == L'\0') {
      return {};
    }
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 1) {
      return {};
    }
    std::string out(static_cast<std::size_t>(utf8_len - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), utf8_len - 1, nullptr, nullptr) <= 0) {
      return {};
    }
    return out;
  }

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

  struct com_scope_t {
    bool should_uninit {false};

    com_scope_t() {
      const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      should_uninit = (hr == S_OK || hr == S_FALSE);
    }

    ~com_scope_t() {
      if (should_uninit) {
        CoUninitialize();
      }
    }
  };

  bool find_virtual_mic_render_device(IMMDevice **out_device, std::string &out_name) {
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator),
      nullptr,
      CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator),
      reinterpret_cast<void **>(&enumerator)
    );
    if (FAILED(hr) || enumerator == nullptr) {
      return false;
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || collection == nullptr) {
      enumerator->Release();
      return false;
    }

    UINT count = 0;
    collection->GetCount(&count);

    std::vector<std::string> preferred;
    if (const char *env = std::getenv("STREAM_DESK_MIC_SINK"); env != nullptr && env[0] != '\0') {
      preferred.emplace_back(env);
    } else {
      preferred.emplace_back("Steam Streaming Microphone");
      preferred.emplace_back("CABLE Input");
      preferred.emplace_back("VB-Audio");
    }

    IMMDevice *matched_device = nullptr;
    std::string matched_name;
    std::size_t best_rank = preferred.size();

    for (UINT i = 0; i < count; ++i) {
      IMMDevice *dev = nullptr;
      if (FAILED(collection->Item(i, &dev)) || dev == nullptr) {
        continue;
      }

      IPropertyStore *props = nullptr;
      std::string friendly_name;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props != nullptr) {
        PROPVARIANT var {};
        PropVariantInit(&var);
        if (SUCCEEDED(props->GetValue(kFriendlyNameKey, &var)) && var.vt == VT_LPWSTR) {
          friendly_name = wide_to_utf8(var.pwszVal);
        }
        PropVariantClear(&var);
        props->Release();
      }

      std::size_t rank = preferred.size();
      for (std::size_t r = 0; r < preferred.size(); ++r) {
        if (icontains(friendly_name, preferred[r])) {
          rank = r;
          break;
        }
      }

      if (rank < best_rank) {
        if (matched_device != nullptr) {
          matched_device->Release();
        }
        matched_device = dev;
        matched_name = std::move(friendly_name);
        best_rank = rank;
        if (best_rank == 0) {
          break;
        }
      } else {
        dev->Release();
      }
    }

    collection->Release();
    enumerator->Release();

    if (matched_device == nullptr) {
      return false;
    }

    out_name = std::move(matched_name);
    if (out_device != nullptr) {
      *out_device = matched_device;
    } else {
      matched_device->Release();
    }
    return true;
  }

  class wasapi_mic_sink_t final : public mic::sink_t {
  public:
    wasapi_mic_sink_t(IMMDevice *device, std::string device_name):
        com_(),
        device_(device),
        device_name_(std::move(device_name)) {
      open_client();
    }

    ~wasapi_mic_sink_t() override {
      close();
    }

    bool is_open() const {
      std::lock_guard<std::mutex> lg(mutex_);
      return !closed_ && render_client_ != nullptr;
    }

    bool write_pcm(const int16_t *samples, std::size_t count) override {
      if (samples == nullptr || count == 0) {
        return true;
      }

      std::lock_guard<std::mutex> lg(mutex_);
      if (closed_ || audio_client_ == nullptr || render_client_ == nullptr) {
        return false;
      }

      UINT32 padding = 0;
      if (FAILED(audio_client_->GetCurrentPadding(&padding))) {
        return false;
      }
      if (buffer_frames_ <= padding) {
        // Drop excess frames when endpoint falls behind to preserve real-time mic latency.
        return true;
      }

      const UINT32 available = buffer_frames_ - padding;
      const UINT32 frames = static_cast<UINT32>(std::min<std::size_t>(count, available));
      if (frames == 0) {
        return true;
      }

      BYTE *dst = nullptr;
      if (FAILED(render_client_->GetBuffer(frames, &dst)) || dst == nullptr) {
        return false;
      }
      std::memcpy(dst, samples, static_cast<std::size_t>(frames) * sizeof(int16_t) * LI_MIC_CHANNELS);
      return SUCCEEDED(render_client_->ReleaseBuffer(frames, 0));
    }

    void close() override {
      std::lock_guard<std::mutex> lg(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;

      if (audio_client_ != nullptr) {
        if (started_) {
          audio_client_->Stop();
          started_ = false;
        }
      }
      if (render_client_ != nullptr) {
        render_client_->Release();
        render_client_ = nullptr;
      }
      if (audio_client_ != nullptr) {
        audio_client_->Release();
        audio_client_ = nullptr;
      }
      if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
      }
    }

  private:
    void open_client() {
      if (device_ == nullptr) {
        return;
      }

      HRESULT hr = device_->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void **>(&audio_client_)
      );
      if (FAILED(hr) || audio_client_ == nullptr) {
        BOOST_LOG(warning) << "WASAPI reverse mic Activate(IAudioClient) failed: 0x" << std::hex << hr;
        close();
        return;
      }

      WAVEFORMATEX wfx {};
      wfx.wFormatTag = WAVE_FORMAT_PCM;
      wfx.nChannels = LI_MIC_CHANNELS;
      wfx.nSamplesPerSec = LI_MIC_SAMPLE_RATE;
      wfx.wBitsPerSample = 16;
      wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
      wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

      const REFERENCE_TIME buffer_duration = 1000000;  // 100ms in 100ns units
      const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
      hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        buffer_duration,
        0,
        &wfx,
        nullptr
      );
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "WASAPI reverse mic Initialize failed for [" << device_name_ << "]: 0x" << std::hex << hr;
        close();
        return;
      }

      hr = audio_client_->GetBufferSize(&buffer_frames_);
      if (FAILED(hr) || buffer_frames_ == 0) {
        close();
        return;
      }

      hr = audio_client_->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void **>(&render_client_)
      );
      if (FAILED(hr) || render_client_ == nullptr) {
        close();
        return;
      }

      hr = audio_client_->Start();
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "WASAPI reverse mic Start failed for [" << device_name_ << "]: 0x" << std::hex << hr;
        close();
        return;
      }
      started_ = true;
      BOOST_LOG(info) << "WASAPI reverse mic sink active on [" << device_name_ << "]";
    }

    com_scope_t com_;
    mutable std::mutex mutex_;
    IMMDevice *device_ {nullptr};
    IAudioClient *audio_client_ {nullptr};
    IAudioRenderClient *render_client_ {nullptr};
    std::string device_name_;
    UINT32 buffer_frames_ {0};
    bool started_ {false};
    bool closed_ {false};
  };

}  // namespace

namespace platf {

  bool mic_available() {
    com_scope_t com;
    std::string name;
    return find_virtual_mic_render_device(nullptr, name);
  }

  std::unique_ptr<mic::sink_t> create_mic_sink() {
    auto sink_com = std::make_unique<com_scope_t>();
    IMMDevice *device = nullptr;
    std::string name;
    if (!find_virtual_mic_render_device(&device, name) || device == nullptr) {
      BOOST_LOG(warning) << "Windows reverse mic render endpoint unavailable";
      return nullptr;
    }

    // wasapi_mic_sink_t owns its own MTA COM scope after construction; release temporary lookup scope first.
    auto sink = std::make_unique<wasapi_mic_sink_t>(device, std::move(name));
    sink_com.reset();
    if (!sink->is_open()) {
      return nullptr;
    }
    return sink;
  }

}  // namespace platf

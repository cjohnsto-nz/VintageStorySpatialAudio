#include "backend/spatial_output.hpp"

#include "core/error.hpp"
#include "core/log.hpp"

#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>

#include <miniaudio.h>  // ma_device_id: DeviceOutput's ids carry the WASAPI endpoint id

#include <algorithm>
#include <array>
#endif

namespace vsa::backend {

#ifdef _WIN32

namespace {

using Microsoft::WRL::ComPtr;

// The 7.1.4 bed in the engine's channel order (FL FR FC LFE BL BR SL SR TFL TFR TBL TBR).
constexpr std::array<AudioObjectType, 12> kBed = {
    AudioObjectType_FrontLeft,    AudioObjectType_FrontRight,    AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency, AudioObjectType_BackLeft,      AudioObjectType_BackRight,
    AudioObjectType_SideLeft,     AudioObjectType_SideRight,     AudioObjectType_TopFrontLeft,
    AudioObjectType_TopFrontRight, AudioObjectType_TopBackLeft,  AudioObjectType_TopBackRight,
};
constexpr uint32_t kChannels = static_cast<uint32_t>(kBed.size());

// A stream that stops signalling is gone (seen on an AV receiver: a timeout, then Reset failing
// with SPTLAUDCLNT_E_DESTROYED); normal updates come every 10 ms.
constexpr DWORD kWaitTimeoutMs = 500;

// PKEY_Device_FriendlyName, spelled out rather than instantiated through initguid.h.
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                       14};

std::string hresult(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

std::string utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

bool is_float_mono(const WAVEFORMATEX& format) {
    if (format.nChannels != 1 || format.wBitsPerSample != 32) {
        return false;
    }
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        // The subformat GUID's first field is the format tag (KSDATAFORMAT_SUBTYPE_IEEE_FLOAT).
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

}  // namespace

struct SpatialOutput::Impl {
    Format format;
    std::thread thread;
    bool open = false;
    HANDLE event = nullptr;
    std::wstring endpoint_id;  // empty: the default device, followed when it changes
    std::wstring active_id;    // the endpoint actually opened

    std::atomic<bool> stop{false};
    std::atomic<bool> lost{false};
    std::atomic<bool> rerouted{false};

    // Start-up handshake between open() and the thread.
    std::mutex mutex;
    std::condition_variable ready;
    enum class Start { Pending, Running, Failed } start = Start::Pending;
    vsa_result error_code = VSA_OK;
    std::string error;

    void fail(vsa_result code, std::string message) {
        std::lock_guard lock(mutex);
        start = Start::Failed;
        error_code = code;
        error = std::move(message);
        ready.notify_all();
    }

    void run(DeviceOutput::RenderFn render, DeviceOutput::PrepareFn prepare, void* user) noexcept;
    void render_loop(ISpatialAudioObjectRenderStream* stream, std::array<ComPtr<ISpatialAudioObject>, 12>& objects,
                     std::vector<float>& scratch, uint32_t max_frames, DeviceOutput::RenderFn render,
                     void* user) noexcept;
};

namespace {

/// Device hot-plug: the default device changing (when following it) or ours going away.
class Notifications final : public IMMNotificationClient {
public:
    explicit Notifications(SpatialOutput::Impl& impl) : impl_(impl) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long refs = --refs_;
        if (refs == 0) {
            delete this;
        }
        return static_cast<ULONG>(refs);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*id*/) override {
        if (flow == eRender && role == eConsole && impl_.endpoint_id.empty()) {
            impl_.rerouted.store(true, std::memory_order_release);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (id != nullptr && impl_.active_id == id && state != DEVICE_STATE_ACTIVE) {
            impl_.lost.store(true, std::memory_order_release);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*id*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*id*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*id*/, const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

private:
    ~Notifications() = default;

    SpatialOutput::Impl& impl_;
    std::atomic<long> refs_{1};
};

}  // namespace

void SpatialOutput::Impl::run(DeviceOutput::RenderFn render, DeviceOutput::PrepareFn prepare, void* user) noexcept {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co)) {
        fail(VSA_ERROR_DEVICE, "COM initialisation failed: " + hresult(co));
        return;
    }
    try {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            throw Error(VSA_ERROR_DEVICE, "creating the device enumerator failed: " + hresult(hr));
        }
        ComPtr<IMMDevice> device;
        hr = endpoint_id.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
                                 : enumerator->GetDevice(endpoint_id.c_str(), &device);
        if (FAILED(hr)) {
            throw Error(VSA_ERROR_DEVICE, "finding the device failed: " + hresult(hr));
        }
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            active_id = id;
            CoTaskMemFree(id);
        }
        std::string name = "unknown device";
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) {
                name = utf8(value.pwszVal);
            }
            PropVariantClear(&value);  // ours: GetValue allocated it
        }

        ComPtr<ISpatialAudioClient> client;
        hr = device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) {
            throw Error(VSA_ERROR_UNSUPPORTED, "'" + name + "' has no spatial audio (" + hresult(hr) + ")");
        }
        hr = client->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
        if (FAILED(hr)) {
            throw Error(VSA_ERROR_UNSUPPORTED, "no spatial sound format is enabled for '" + name + "' (" +
                                                   hresult(hr) + "; Windows sound settings, Spatial sound)");
        }

        // The object format: the first one the endpoint offers (float32 mono in practice). Copied,
        // because the enumerator owns it and the activation parameters must point at it.
        ComPtr<IAudioFormatEnumerator> formats;
        WAVEFORMATEX* offered = nullptr;
        hr = client->GetSupportedAudioObjectFormatEnumerator(&formats);
        if (SUCCEEDED(hr)) {
            hr = formats->GetFormat(0, &offered);
        }
        if (FAILED(hr) || offered == nullptr) {
            throw Error(VSA_ERROR_DEVICE, "reading the spatial object format failed: " + hresult(hr));
        }
        WAVEFORMATEXTENSIBLE object_format{};
        std::memcpy(&object_format, offered,
                    std::min(sizeof object_format, sizeof(WAVEFORMATEX) + static_cast<std::size_t>(offered->cbSize)));
        const WAVEFORMATEX& wave = object_format.Format;
        if (!is_float_mono(wave) || (wave.nSamplesPerSec != 44100 && wave.nSamplesPerSec != 48000)) {
            throw Error(VSA_ERROR_UNSUPPORTED, "unsupported spatial object format (" +
                                                   std::to_string(wave.nChannels) + " ch, " +
                                                   std::to_string(wave.wBitsPerSample) + " bit, " +
                                                   std::to_string(wave.nSamplesPerSec) + " Hz)");
        }
        UINT32 max_frames = 0;
        hr = client->GetMaxFrameCount(&wave, &max_frames);
        if (FAILED(hr) || max_frames == 0) {
            throw Error(VSA_ERROR_DEVICE, "reading the spatial frame count failed: " + hresult(hr));
        }
        AudioObjectType native_mask = AudioObjectType_None;
        (void)client->GetNativeStaticObjectTypeMask(&native_mask);
        UINT32 max_dynamic = 0;
        (void)client->GetMaxDynamicObjectCount(&max_dynamic);

        AudioObjectType mask = AudioObjectType_None;
        for (const AudioObjectType type : kBed) {
            mask = static_cast<AudioObjectType>(mask | type);
        }
        SpatialAudioObjectRenderStreamActivationParams params{};
        params.ObjectFormat = &wave;
        params.StaticObjectTypeMask = mask;
        params.MinDynamicObjectCount = 0;
        params.MaxDynamicObjectCount = 0;
        params.Category = AudioCategory_GameEffects;
        params.EventHandle = event;
        params.NotifyObject = nullptr;
        // The blob borrows `params` (and the format it points to), which stay ours. So this
        // PROPVARIANT must never go through PropVariantClear: that would CoTaskMemFree stack
        // memory (OpenAL Soft's heap corruption, 0xc0000374, on this same API).
        PROPVARIANT activation;
        PropVariantInit(&activation);
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof params;
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        ComPtr<ISpatialAudioObjectRenderStream> stream;
        hr = client->ActivateSpatialAudioStream(&activation, IID_PPV_ARGS(&stream));
        if (FAILED(hr)) {
            throw Error(VSA_ERROR_DEVICE, "activating the spatial stream failed: " + hresult(hr));
        }

        std::array<ComPtr<ISpatialAudioObject>, 12> objects;
        for (uint32_t c = 0; c < kChannels; ++c) {
            hr = stream->ActivateSpatialAudioObject(kBed[c], &objects[c]);
            if (FAILED(hr)) {
                throw Error(VSA_ERROR_DEVICE, "activating bed object " + std::to_string(c) + " failed: " + hresult(hr));
            }
        }

        format.sample_rate = wave.nSamplesPerSec;
        format.channels = kChannels;
        format.period_frames = max_frames;
        format.name = name;
        const auto layout = steam_layout(kChannels);
        format.speakers.assign(layout.begin(), layout.begin() + kChannels);
        prepare(user, format.sample_rate, format.channels, format.speakers.data());
        std::vector<float> scratch(static_cast<std::size_t>(max_frames) * kChannels, 0.0f);

        ComPtr<Notifications> notifications;
        notifications.Attach(new Notifications(*this));
        const bool registered = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(notifications.Get()));

        hr = stream->Start();
        if (FAILED(hr)) {
            if (registered) {
                enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
            }
            throw Error(VSA_ERROR_DEVICE, "starting the spatial stream failed: " + hresult(hr));
        }
        Log::writef(VSA_LOG_INFO,
                    "spatial output: '%s', %u Hz, 7.1.4 bed, %u-frame updates (native static objects 0x%X, "
                    "dynamic objects up to %u)",
                    name.c_str(), format.sample_rate, max_frames, static_cast<unsigned>(native_mask), max_dynamic);
        {
            std::lock_guard lock(mutex);
            start = Start::Running;
            ready.notify_all();
        }

        DWORD task_index = 0;
        const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        render_loop(stream.Get(), objects, scratch, max_frames, render, user);
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        stream->Stop();
        if (registered) {
            enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
        }
    } catch (const Error& e) {
        fail(e.code(), e.what());
    } catch (const std::exception& e) {
        fail(VSA_ERROR_DEVICE, e.what());
    }
    CoUninitialize();
}

void SpatialOutput::Impl::render_loop(ISpatialAudioObjectRenderStream* stream,
                                      std::array<ComPtr<ISpatialAudioObject>, 12>& objects, std::vector<float>& scratch,
                                      uint32_t max_frames, DeviceOutput::RenderFn render, void* user) noexcept {
    // The real-time loop: no allocation, locks or logging (failures only raise `lost`).
    float* interleaved = scratch.data();
    while (!stop.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(event, kWaitTimeoutMs) != WAIT_OBJECT_0) {
            lost.store(true, std::memory_order_release);
            return;
        }
        if (stop.load(std::memory_order_acquire)) {
            return;
        }
        UINT32 dynamic = 0;
        UINT32 frames = 0;
        if (FAILED(stream->BeginUpdatingAudioObjects(&dynamic, &frames))) {
            lost.store(true, std::memory_order_release);
            return;
        }
        frames = std::min(frames, max_frames);
        if (frames > 0) {
            render(user, interleaved, frames);
        }
        for (uint32_t c = 0; c < kChannels; ++c) {
            BYTE* buffer = nullptr;
            UINT32 bytes = 0;
            if (FAILED(objects[c]->GetBuffer(&buffer, &bytes)) || buffer == nullptr) {
                continue;
            }
            auto* out = reinterpret_cast<float*>(buffer);
            const uint32_t n = std::min(frames, static_cast<uint32_t>(bytes / sizeof(float)));
            for (uint32_t j = 0; j < n; ++j) {
                out[j] = interleaved[static_cast<std::size_t>(j) * kChannels + c];
            }
        }
        if (FAILED(stream->EndUpdatingAudioObjects())) {
            lost.store(true, std::memory_order_release);
            return;
        }
    }
}

SpatialOutput::SpatialOutput() : impl_(std::make_unique<Impl>()) {}

SpatialOutput::~SpatialOutput() { close(); }

DeviceOutput::Format SpatialOutput::open(const vsa_device_id* id, DeviceOutput::RenderFn render,
                                         DeviceOutput::PrepareFn prepare, void* user) {
    close();
    Impl& s = *impl_;
    s.endpoint_id.clear();
    s.active_id.clear();
    if (id != nullptr) {
        ma_device_id device{};
        std::memcpy(&device, id->bytes, sizeof device);
        const auto* text = reinterpret_cast<const wchar_t*>(device.wasapi);
        s.endpoint_id.assign(text, wcsnlen(text, sizeof device.wasapi / sizeof(wchar_t)));
    }
    s.stop.store(false);
    s.lost.store(false);
    s.rerouted.store(false);
    s.start = Impl::Start::Pending;
    s.error.clear();
    s.format = Format{};
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s.event == nullptr) {
        throw Error(VSA_ERROR_DEVICE, "creating the spatial stream event failed");
    }

    s.thread = std::thread([&s, render, prepare, user] { s.run(render, prepare, user); });
    std::unique_lock lock(s.mutex);
    s.ready.wait(lock, [&s] { return s.start != Impl::Start::Pending; });
    if (s.start == Impl::Start::Failed) {
        lock.unlock();
        s.thread.join();
        CloseHandle(s.event);
        s.event = nullptr;
        throw Error(s.error_code, s.error);
    }
    s.open = true;
    return s.format;
}

void SpatialOutput::close() noexcept {
    Impl& s = *impl_;
    if (!s.thread.joinable()) {
        return;
    }
    s.stop.store(true, std::memory_order_release);
    SetEvent(s.event);  // wake the loop
    s.thread.join();
    CloseHandle(s.event);
    s.event = nullptr;
    s.open = false;
    s.format = Format{};
}

bool SpatialOutput::is_open() const noexcept { return impl_->open; }
const DeviceOutput::Format& SpatialOutput::format() const noexcept { return impl_->format; }
bool SpatialOutput::take_lost() noexcept { return impl_->lost.exchange(false, std::memory_order_acq_rel); }
bool SpatialOutput::take_rerouted() noexcept { return impl_->rerouted.exchange(false, std::memory_order_acq_rel); }

#else  // !_WIN32

struct SpatialOutput::Impl {
    Format format;
};

SpatialOutput::SpatialOutput() : impl_(std::make_unique<Impl>()) {}
SpatialOutput::~SpatialOutput() = default;

DeviceOutput::Format SpatialOutput::open(const vsa_device_id* /*id*/, DeviceOutput::RenderFn /*render*/,
                                         DeviceOutput::PrepareFn /*prepare*/, void* /*user*/) {
    throw Error(VSA_ERROR_UNSUPPORTED, "spatial audio output is only available on Windows");
}

void SpatialOutput::close() noexcept {}
bool SpatialOutput::is_open() const noexcept { return false; }
const DeviceOutput::Format& SpatialOutput::format() const noexcept { return impl_->format; }
bool SpatialOutput::take_lost() noexcept { return false; }
bool SpatialOutput::take_rerouted() noexcept { return false; }

#endif

}  // namespace vsa::backend

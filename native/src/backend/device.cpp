#include "backend/device.hpp"

#include "core/error.hpp"
#include "core/log.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace vsa::backend {
namespace {

static_assert(sizeof(ma_device_id) <= sizeof(vsa_device_id), "ma_device_id must fit in vsa_device_id");
static_assert(sizeof(ma_uint32) == sizeof(uint32_t));

void data_trampoline(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    static_cast<DeviceOutput*>(device->pUserData)->on_render(static_cast<float*>(output), frames);
}

void notification_trampoline(const ma_device_notification* notification) {
    auto* self = static_cast<DeviceOutput*>(notification->pDevice->pUserData);
    switch (notification->type) {
        case ma_device_notification_type_stopped: self->on_stopped(); break;
        case ma_device_notification_type_rerouted: self->on_rerouted(); break;
        default: break;
    }
}

// Layouts the engine renders: stereo, quad, 5.1 and 7.1.
uint32_t supported_channels(uint32_t channels) noexcept {
    if (channels <= 2) {
        return 2;
    }
    if (channels >= 8) {
        return 8;
    }
    return channels & ~1u;
}

std::string describe(ma_result result) { return std::string(ma_result_description(result)); }

}  // namespace

void DeviceOutput::ContextDeleter::operator()(ma_context* context) const noexcept {
    ma_context_uninit(context);
    delete context;
}

void DeviceOutput::DeviceDeleter::operator()(ma_device* device) const noexcept {
    ma_device_uninit(device);  // stops the device and waits for a running callback to return
    delete device;
}

DeviceOutput::DeviceOutput() = default;

DeviceOutput::~DeviceOutput() {
    close();
    context_.reset();
}

void DeviceOutput::ensure_context() {
    if (context_) {
        return;
    }
    ma_context_config config = ma_context_config_init();
    config.threadPriority = ma_thread_priority_realtime;
    auto* context = new ma_context;
    if (const ma_result result = ma_context_init(nullptr, 0, &config, context); result != MA_SUCCESS) {
        delete context;
        throw Error(VSA_ERROR_DEVICE, "audio backend initialisation failed: " + describe(result));
    }
    context_.reset(context);
    Log::writef(VSA_LOG_INFO, "audio backend: %s", ma_get_backend_name(context_->backend));
}

std::vector<vsa_device_info> DeviceOutput::enumerate() {
    ensure_context();
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    if (const ma_result result = ma_context_get_devices(context_.get(), &playback, &count, nullptr, nullptr);
        result != MA_SUCCESS) {
        throw Error(VSA_ERROR_DEVICE, "device enumeration failed: " + describe(result));
    }
    std::vector<vsa_device_info> devices(count);
    for (ma_uint32 i = 0; i < count; ++i) {
        vsa_device_info& info = devices[i];
        info.struct_size = sizeof info;
        info.is_default = playback[i].isDefault != 0 ? 1u : 0u;
        const char* name = playback[i].name;
        const auto length =
            static_cast<std::size_t>(std::find(name, name + sizeof info.name - 1, '\0') - name);
        std::memcpy(info.name, playback[i].name, length);
        info.name[length] = '\0';
        std::memcpy(info.id.bytes, &playback[i].id, sizeof(ma_device_id));
    }
    return devices;
}

std::unique_ptr<ma_device, DeviceOutput::DeviceDeleter> DeviceOutput::init_device(const vsa_device_id* id,
                                                                                  uint32_t channels, uint32_t rate) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    ma_device_id device_id{};
    if (id != nullptr) {
        std::memcpy(&device_id, id->bytes, sizeof device_id);
        config.playback.pDeviceID = &device_id;
    }
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = rate;  // 0 = the device's native rate
    config.dataCallback = &data_trampoline;
    config.notificationCallback = &notification_trampoline;
    config.pUserData = this;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;  // we write every frame
    config.noClip = MA_TRUE;                     // the limiter keeps us in range
    config.noFixedSizedCallback = MA_TRUE;       // the mixer has its own block FIFO

    auto* device = new ma_device;
    if (const ma_result result = ma_device_init(context_.get(), &config, device); result != MA_SUCCESS) {
        delete device;
        throw Error(VSA_ERROR_DEVICE, "opening the audio device failed: " + describe(result));
    }
    return std::unique_ptr<ma_device, DeviceDeleter>(device);
}

DeviceOutput::Format DeviceOutput::open(const vsa_device_id* id, uint32_t channels, RenderFn render,
                                        PrepareFn prepare, void* user) {
    close();
    ensure_context();
    render_ = render;
    user_ = user;
    lost_.store(false);
    rerouted_.store(false);
    closing_.store(false);

    // The engine renders at 44.1 or 48 kHz (Steam Audio's HRTF supports no other useful rates);
    // any other native rate gets the nearest family and miniaudio converts.
    auto device = init_device(id, channels, 0);
    const uint32_t native_rate = device->sampleRate;
    const uint32_t rate = native_rate == 44100 || native_rate == 48000 ? native_rate
                          : native_rate % 44100 == 0                  ? 44100u
                                                                       : 48000u;
    const uint32_t wanted = channels == 0 ? supported_channels(device->playback.channels) : channels;
    if (device->playback.channels != wanted || rate != native_rate) {
        device.reset();
        device = init_device(id, wanted, rate);
        Log::writef(VSA_LOG_INFO, "device runs at %u Hz; rendering at %u Hz", native_rate, rate);
    }

    Format format;
    format.sample_rate = device->sampleRate;
    format.channels = device->playback.channels;
    format.period_frames = device->playback.internalPeriodSizeInFrames;
    format.name = device->playback.name;
    prepare(user, format.sample_rate, format.channels);

    if (const ma_result result = ma_device_start(device.get()); result != MA_SUCCESS) {
        closing_.store(true);
        throw Error(VSA_ERROR_DEVICE, "starting the audio device failed: " + describe(result));
    }
    device_ = std::move(device);
    format_ = format;
    Log::writef(VSA_LOG_INFO, "output: '%s', %u Hz, %u channels, period %u frames", format_.name.c_str(),
                format_.sample_rate, format_.channels, format_.period_frames);
    return format_;
}

void DeviceOutput::close() noexcept {
    if (!device_) {
        return;
    }
    closing_.store(true, std::memory_order_release);
    device_.reset();
    format_ = Format{};
}

}  // namespace vsa::backend

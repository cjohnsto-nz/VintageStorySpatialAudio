#pragma once

#include "core/thread_stats.hpp"

#include "audio/channel_layout.hpp"
#include "vsaudio.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ma_context;
struct ma_device;

namespace vsa::backend {

/// Playback through miniaudio (WASAPI shared mode, CoreAudio, PipeWire/PulseAudio/ALSA).
///
/// The device callback is the engine's render thread. miniaudio's context is created with
/// real-time thread priority. Hot-plug: "stopped" (device lost) and "rerouted" (followed a new
/// default device) notifications only set flags; the engine's worker polls them and reopens the
/// device from its own thread (a device must never be torn down from its own callback).
///
/// Not thread-safe: the engine serialises every call with its output lock.
class DeviceOutput {
public:
    using RenderFn = void (*)(void* user, float* out, uint32_t frames) noexcept;
    /// Called after the device is initialised and before it starts, with its actual format and
    /// channel order (`channels` entries).
    using PrepareFn = void (*)(void* user, uint32_t sample_rate, uint32_t channels, const Speaker* speakers);

    struct Format {
        uint32_t sample_rate = 0;
        uint32_t channels = 0;
        uint32_t period_frames = 0;
        std::string name;
        std::vector<Speaker> speakers;
    };

    DeviceOutput();
    ~DeviceOutput();

    DeviceOutput(const DeviceOutput&) = delete;
    DeviceOutput& operator=(const DeviceOutput&) = delete;

    /// Playback devices (creates the backend context on first use). Throws vsa::Error.
    std::vector<vsa_device_info> enumerate();

    /// Opens and starts a device. `id` null = system default, followed when it changes.
    /// `channels` 0 = native layout, clamped to 2/4/6/8/12. Throws vsa::Error (VSA_ERROR_DEVICE).
    Format open(const vsa_device_id* id, uint32_t channels, RenderFn render, PrepareFn prepare, void* user);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return device_ != nullptr; }
    [[nodiscard]] const Format& format() const noexcept { return format_; }

    /// Returns and clears the corresponding notification flag.
    bool take_lost() noexcept { return lost_.exchange(false, std::memory_order_acq_rel); }
    bool take_rerouted() noexcept { return rerouted_.exchange(false, std::memory_order_acq_rel); }

    // Entry points for miniaudio's callbacks (device.cpp trampolines).
    void on_render(float* out, uint32_t frames) noexcept {
        render_thread_id_.store(ThreadRegistry::current_id(), std::memory_order_relaxed);
        render_(user_, out, frames);
    }
    /// The id of the thread the device calls back on (0 before the first callback); the engine
    /// worker announces it to the thread registry, since the callback itself must not allocate.
    [[nodiscard]] uint32_t render_thread_id() const noexcept { return render_thread_id_.load(std::memory_order_relaxed); }
    void on_stopped() noexcept {
        if (!closing_.load(std::memory_order_acquire)) {
            lost_.store(true, std::memory_order_release);
        }
    }
    void on_rerouted() noexcept { rerouted_.store(true, std::memory_order_release); }

private:
    struct ContextDeleter {
        void operator()(ma_context* context) const noexcept;
    };
    struct DeviceDeleter {
        void operator()(ma_device* device) const noexcept;
    };

    void ensure_context();
    std::unique_ptr<ma_device, DeviceDeleter> init_device(const vsa_device_id* id, uint32_t channels, uint32_t rate);

    std::unique_ptr<ma_context, ContextDeleter> context_;
    std::unique_ptr<ma_device, DeviceDeleter> device_;
    Format format_;

    RenderFn render_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> closing_{false};
    std::atomic<bool> lost_{false};
    std::atomic<uint32_t> render_thread_id_{0};
    std::atomic<bool> rerouted_{false};
};

}  // namespace vsa::backend

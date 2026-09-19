#pragma once

#include "backend/device.hpp"
#include "vsaudio.h"

#include <atomic>
#include <memory>

namespace vsa::backend {

/// Playback through Windows Spatial Audio (ISpatialAudioClient): the engine's 7.1.4 mix feeds
/// the twelve static bed objects of a spatial render stream, which Windows hands to the active
/// spatial format (Dolby Atmos for home theater or headphones, DTS:X, Windows Sonic). Heights
/// reach an AV receiver this way; plain WASAPI over HDMI stops at 8 channels.
///
/// The backend owns a thread (COM multithreaded apartment, MMCSS "Pro Audio") that activates the
/// stream and then runs the render loop: wait for the stream's event, BeginUpdatingAudioObjects,
/// render the frames it asks for, copy each channel into its object's buffer, End. A wait that
/// times out or a failing update marks the output lost (the stream is gone; a Reset does not
/// bring it back), and the engine's worker recreates it or falls back to the plain device.
///
/// Elsewhere than Windows, open() fails with VSA_ERROR_UNSUPPORTED.
///
/// Not thread-safe: the engine serialises every call with its output lock.
class SpatialOutput {
public:
    using Format = DeviceOutput::Format;

    SpatialOutput();
    ~SpatialOutput();

    SpatialOutput(const SpatialOutput&) = delete;
    SpatialOutput& operator=(const SpatialOutput&) = delete;

    /// Opens a spatial stream on the device (`id` as enumerated by DeviceOutput; null = the
    /// default device, followed when it changes) and starts it. Always 7.1.4 (12 channels, the
    /// engine's order). Throws vsa::Error: VSA_ERROR_UNSUPPORTED when the platform or the device
    /// has no spatial audio (no spatial format enabled for it), VSA_ERROR_DEVICE otherwise.
    Format open(const vsa_device_id* id, DeviceOutput::RenderFn render, DeviceOutput::PrepareFn prepare, void* user);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const Format& format() const noexcept;

    /// Returns and clears the corresponding notification flag. Rerouted: the default device
    /// changed while following it; the stream must be reopened (the engine does it).
    bool take_lost() noexcept;
    bool take_rerouted() noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace vsa::backend

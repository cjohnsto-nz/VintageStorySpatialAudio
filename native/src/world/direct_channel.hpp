#pragma once

#include "vsaudio.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace vsa::world {

/// Render thread -> simulation, one per effect set. `generation` 0 = not simulated; the render
/// thread bumps it whenever the set changes voices, so results for an earlier voice are ignored.
struct DirectInput {
    std::atomic<uint32_t> generation{0};
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> z{0.0f};
    std::atomic<float> radius{0.5f};
    std::atomic<vsa_voice> voice{0};
};

/// Simulation -> render thread, one per effect set; valid for inputs of the same generation.
/// The values are written before the generation (release), and read after it (acquire).
struct DirectOutput {
    std::atomic<uint32_t> generation{0};
    std::atomic<float> occlusion{1.0f};
    std::atomic<float> transmission[3] = {1.0f, 1.0f, 1.0f};
    /// Metres from the listener through the air (around obstacles), -1 if there is no such path.
    std::atomic<float> air_path{-1.0f};
};

/// The lock-free slots between the render thread and the direct simulation.
class DirectChannel {
public:
    explicit DirectChannel(uint32_t size)
        : size_(size), inputs_(std::make_unique<DirectInput[]>(size)), outputs_(std::make_unique<DirectOutput[]>(size)) {}

    [[nodiscard]] uint32_t size() const noexcept { return size_; }

    /// Set by the simulation each tick: whether the scene holds any chunks. Without geometry
    /// nothing can be in the way, so new voices need not wait for a result.
    std::atomic<bool> scene_has_chunks{false};
    [[nodiscard]] DirectInput& input(uint32_t i) noexcept { return inputs_[i]; }
    [[nodiscard]] DirectOutput& output(uint32_t i) noexcept { return outputs_[i]; }

private:
    uint32_t size_;
    std::unique_ptr<DirectInput[]> inputs_;
    std::unique_ptr<DirectOutput[]> outputs_;
};

}  // namespace vsa::world

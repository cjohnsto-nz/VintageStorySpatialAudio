#pragma once

#include "vsaudio.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace vsa::world {

/// Render thread -> pathing simulation, one per effect set (the direct channel's generations).
/// `wanted`: the voice's straight path is blocked, so a way round is worth finding.
struct PathInput {
    std::atomic<uint32_t> generation{0};
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> z{0.0f};
    std::atomic<float> level{0.0f};  // for ranking when more want paths than can be simulated
    std::atomic<bool> wanted{false};
    std::atomic<vsa_voice> voice{0};
};

/// Pathing simulation -> render thread, one per effect set: Steam Audio's path effect
/// parameters. Written before `generation` (release), read after it (acquire). `sh` are the
/// order-1 Ambisonic coefficients (world space) of the paths found, with their attenuation;
/// all zero when there is no path.
struct PathOutput {
    std::atomic<uint32_t> generation{0};
    std::atomic<bool> found{false};
    std::atomic<float> eq[3] = {1.0f, 1.0f, 1.0f};
    std::atomic<float> sh[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

/// The lock-free slots between the render thread and the pathing simulation.
class PathChannel {
public:
    explicit PathChannel(uint32_t size)
        : size_(size), inputs_(std::make_unique<PathInput[]>(size)), outputs_(std::make_unique<PathOutput[]>(size)) {}

    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    [[nodiscard]] PathInput& input(uint32_t i) noexcept { return inputs_[i]; }
    [[nodiscard]] PathOutput& output(uint32_t i) noexcept { return outputs_[i]; }

private:
    uint32_t size_;
    std::unique_ptr<PathInput[]> inputs_;
    std::unique_ptr<PathOutput[]> outputs_;
};

}  // namespace vsa::world

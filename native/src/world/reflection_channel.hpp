#pragma once

#include "vsaudio.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace vsa::world {

/// Render thread -> reflection simulation, one per reflection slot. Slot 0 is the listener's own
/// reverb and is always simulated (at the listener); the others follow the voice the render
/// thread gave them. `generation` 0 = not simulated; bumped whenever the slot changes voices.
struct ReflectionInput {
    std::atomic<uint32_t> generation{0};
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> z{0.0f};
    std::atomic<vsa_voice> voice{0};
};

/// Reflection simulation -> render thread, one per slot. The simulator's impulse response reaches
/// the slot's effect through Steam Audio's own lock-free buffer; these are the numbers that go
/// with it (Steam Audio's hybrid reverb parameters). Values are written before `generation`
/// (release) and read after it (acquire). `observed` is the input generation the latest tick
/// started with: once it is 0 after a slot was released, no further impulse response for the old
/// voice can arrive.
struct ReflectionOutput {
    std::atomic<uint32_t> generation{0};
    std::atomic<uint32_t> observed{0};
    std::atomic<float> reverb_times[3] = {0.0f, 0.0f, 0.0f};
    std::atomic<float> eq[3] = {0.0f, 0.0f, 0.0f};
    std::atomic<int32_t> delay{0};
};

/// The lock-free slots between the render thread and the reflection simulation.
class ReflectionChannel {
public:
    explicit ReflectionChannel(uint32_t size)
        : size_(size),
          inputs_(std::make_unique<ReflectionInput[]>(size)),
          outputs_(std::make_unique<ReflectionOutput[]>(size)) {}

    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    [[nodiscard]] ReflectionInput& input(uint32_t i) noexcept { return inputs_[i]; }
    [[nodiscard]] ReflectionOutput& output(uint32_t i) noexcept { return outputs_[i]; }

    /// The listener slot's generation (constant: it never changes voices).
    static constexpr uint32_t kListenerGeneration = 1;

private:
    uint32_t size_;
    std::unique_ptr<ReflectionInput[]> inputs_;
    std::unique_ptr<ReflectionOutput[]> outputs_;
};

}  // namespace vsa::world

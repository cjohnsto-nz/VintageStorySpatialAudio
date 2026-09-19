#pragma once

#include "audio/speaker_decoder.hpp"
#include "dsp/late_reverb.hpp"
#include "steam/ipl_handle.hpp"
#include "vsaudio.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace vsa::steam {
class SteamContext;
}

namespace vsa::world {
class ReflectionChannel;
class ReflectionSimulator;
}  // namespace vsa::world

namespace vsa {

/// Counters the render thread publishes about the reflections (read by API threads).
struct ReflectionMeter {
    /// Mean square of the reflections' omnidirectional output, smoothed over ~0.3 s.
    std::atomic<float> mean_square{0.0f};
    std::atomic<uint32_t> live_slots{0};
    std::atomic<uint32_t> waiting_slots{0};
    std::atomic<uint32_t> draining_slots{0};
};

/// The render side of the reflections (Phase 6, ADRs 0009 and 0010). One slot per simulated
/// source: slot 0 is the listener's reverb, fed by every world sound without a slot of its own;
/// slots 1.. are given by the mixer to single voices or to spots where short sounds happen. Each
/// slot renders
///  - the early part of its simulated impulse response by convolution (Steam Audio's reflection
///    effect, Ambisonic, reading the simulator's impulse response for the slot), except the
///    listener's: its early reflections are those of a sound at the listener's head, wrong in time
///    for every other sound, and they comb against the direct sound; and
///  - the diffuse tail with our own LateReverb, from the simulated RT60 and level, as plane
///    waves around the listener (the listener's starting sooner, where its early part would be).
/// Everything is summed into a world-space Ambisonic bus, which the mixer decodes: binaurally
/// with the world bus (headphones) or with a SpeakerDecoder.
///
/// A slot handed back is drained (its tail plays out) and only reused once the simulator has
/// confirmed it stopped simulating the old voice, and any impulse response still in flight for
/// it has been consumed: a new voice never gets an old voice's reflections.
///
/// prepare() allocates; everything else is for the render thread and never allocates.
class ReflectionRenderer {
public:
    ReflectionRenderer(const steam::SteamContext& steam, world::ReflectionChannel* channel, uint32_t block_frames);
    ~ReflectionRenderer();

    ReflectionRenderer(const ReflectionRenderer&) = delete;
    ReflectionRenderer& operator=(const ReflectionRenderer&) = delete;

    /// For the output's rate and speaker layout (`channels`), reading impulse responses from
    /// `simulator` (null: disabled). Nothing may render meanwhile.
    void prepare(uint32_t sample_rate, uint32_t channels, world::ReflectionSimulator* simulator);

    [[nodiscard]] bool enabled() const noexcept { return simulator_ != nullptr; }
    [[nodiscard]] uint32_t slot_count() const noexcept { return static_cast<uint32_t>(slots_.size()); }
    [[nodiscard]] ReflectionMeter& meter() noexcept { return meter_; }

    /// Start of a block: clears the sends.
    void begin_block() noexcept;
    /// A voice slot (>= 1) for `voice`, or -1 if none is free.
    int acquire(vsa_voice voice) noexcept;
    /// Hands a voice slot back (it drains, then becomes free).
    void release(int slot) noexcept;
    /// Where the slot's voice is (scene coordinates), for the simulation.
    void set_position(int slot, const float position[3]) noexcept;
    /// Whether the slot has results for its current voice (its own reflections can be heard).
    [[nodiscard]] bool ready(int slot) const noexcept;
    /// The slot's current generation: it changes whenever the slot is given to someone else.
    [[nodiscard]] uint32_t generation(int slot) const noexcept {
        return slots_[static_cast<std::size_t>(slot)].generation;
    }
    /// The slot's input for this block (block_frames samples, accumulated into).
    [[nodiscard]] float* send(int slot) noexcept;
    [[nodiscard]] uint32_t free_slots() const noexcept;

    /// Renders every slot into the bus, times `gain` (per frame). Returns false if the bus is
    /// silent this block.
    bool render(const float* gain) noexcept;
    [[nodiscard]] float* const* bus() const noexcept { return bus_.data(); }
    [[nodiscard]] uint32_t bus_channels() const noexcept { return channels_; }
    /// Adds the bus, decoded for this listener, to `out` (the speaker layout's channels).
    void decode_speakers(const float right[3], const float up[3], const float ahead[3], float* const* out) noexcept;

private:
    enum class State : uint8_t { Free, Waiting, Live, Draining };
    struct Slot {
        steam::ReflectionEffect early;
        dsp::LateReverb late;
        IPLReflectionEffectIR ir = nullptr;
        State state = State::Free;
        uint32_t generation = 0;
        float reverb_times[3] = {};
        float eq[3] = {};
        int32_t delay = 0;
        bool sent = false;
        uint32_t early_tail = 0;   // blocks the convolution still rings after its last input
        bool late_sounding = false;
        bool flushed = false;      // draining: consumed everything the simulator sent after release
        std::vector<float> send;
    };

    /// Takes the simulator's latest parameters for the slot; true if they are for its voice.
    bool take_results(uint32_t index, Slot& slot) noexcept;
    /// Convolves (and, for the late part, reverberates) the slot's input into the bus. `flush`
    /// runs the convolution even when it has stopped ringing (to take a pending response).
    void render_slot(Slot& slot, bool input, bool flush, bool listener) noexcept;

    const steam::SteamContext& steam_;
    world::ReflectionChannel* channel_;
    world::ReflectionSimulator* simulator_ = nullptr;
    const uint32_t frames_;
    uint32_t sample_rate_ = 48000;
    uint32_t channels_ = 0;       // Ambisonic channels of the bus
    uint32_t early_blocks_ = 0;   // blocks the convolution rings after its input stops
    uint32_t listener_predelay_ = 0;  // where the listener's tail starts (samples)
    std::vector<Slot> slots_;
    // Each slot's latest generation, kept across prepare(): after a reopen, results published for
    // a voice from before it can never match a new voice.
    std::vector<uint32_t> generations_;
    std::vector<float> silence_;
    std::vector<float> early_storage_;
    std::array<float*, 16> early_out_{};
    std::vector<float> late_storage_;
    std::array<float*, dsp::LateReverb::kOutputs> late_out_{};
    // The late outputs' plane-wave encodings (bus order), one row per output.
    std::array<std::array<float, 16>, dsp::LateReverb::kOutputs> late_sh_{};
    std::vector<float> bus_storage_;
    std::array<float*, 16> bus_{};
    bool bus_used_ = false;
    std::unique_ptr<SpeakerDecoder> decoder_;
    float mean_square_ = 0.0f;
    float meter_alpha_ = 0.0f;
    ReflectionMeter meter_;
};

}  // namespace vsa

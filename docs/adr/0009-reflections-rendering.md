# ADR 0009: Reflections: a slot pool, convolved early reflections and our own diffuse tail

**Status:** Accepted, 19 Sep 2026. Amends PLAN §5.4.

## Context

PLAN §5.4 has two parts:
- **Listener reverb:** always on.
- **Per-source reflections:** for the top N sources, using Steam Audio's HYBRID effect mixed through an `IPLReflectionMixer` into the Ambisonic bus.

Building it against Steam Audio 4.8.1, and measuring it, showed four problems:

- **The hybrid tail is mono.** The parametric part of `IPL_REFLECTIONEFFECTTYPE_HYBRID` (`ReverbEffect`) has one output, added to the W channel only. Decoded, the late reverb collapses to the middle of the head, or into the same signal on every speaker, instead of surrounding the listener.
- **The hybrid tail is too quiet.** It starts about 6 dB below Steam Audio's own simulated impulse response: the same simulation rendered in full by convolution, measured in rooms of 0.4–2.9 s decay. Steam Audio's parametric network also decays about 12% slower than the RT60 it is given.
- **A reflection mixer can't be used with HYBRID.** Steam Audio's own header says so. Full-length convolution through a mixer costs one multiply-accumulate pass per source over every block of a 1–2 s response, which is too much for the render budget.
- **The cost is IR reconstruction, not ray tracing.** A simulation run is dominated by Steam Audio rebuilding every enabled source's full-length impulse response, about 4 ms per source per second of response at order 2. 4096 rays against 2048 rays changes a run by under 10%.

## Decision

**A fixed pool of reflection slots (`world::ReflectionSimulator`, `ReflectionRenderer`):**
- Slot 0 is a source at the listener. Every world sound without a slot of its own sends to it: `min_distance / sqrt(max(1, d))`, halfway between a diffuse field (no fall-off) and a direct path (-6 dB per doubling).
- Slots 1..N go to the loudest world voices that last: looping, streamed, or at least 0.75 s. They are ranked by level without walls, since reflections are how sound reaches you around them.
- A voice takes another's slot only if it is 6 dB louder.
- A voice cross-fades from the shared reverb to its own over eight blocks once its slot has a result, so short sounds never wait.

**Sources live as long as the simulator:**
- Their impulse-response buffers are what each slot's effect reads.
- A slot that changes voices is only reused after:
  - its old tail has drained;
  - the simulator has run a tick without the old voice (`observed == 0`);
  - one more pass through the effect has consumed any response still in Steam Audio's triple buffer.

**Rendering per slot:**
- **Early part:** Steam Audio's CONVOLUTION effect over the first `transition` seconds (0.1 s by default) of the simulated response, which the HYBRID simulation windows and silences after. Directional, Ambisonic, and cheap: 19 partitions, not 190.
- **Late part:** our own `dsp::LateReverb`:
  - a 16-line feedback delay network with first-order shelves that decay at the simulated RT60 per band;
  - fed through Steam Audio's delay and per-band level ("EQ");
  - eight outputs, each a pair of lines, so they are independent and every line reaches the pressure;
  - encoded as plane waves from the corners of a cube, so the tail surrounds the listener.
- **Tail level:** calibrated against the full convolution response: `level × 0.635 × RT60^-0.385`, because Steam Audio's EQ undershoots more the shorter the decay. Within 0.7 dB in the rooms it was fitted on and 2.5 dB in the rooms checked (`core/test_reflection_calibration.cpp`).

**Decoding:**
- Everything sums into a world-space Ambisonic bus of the simulation's order.
- **Headphones:** the bus joins the existing world bus and its binaural decode, which now runs only at the highest order present.
- **Speakers:** our `SpeakerDecoder`, AllRAD. 64 virtual speakers with max-rE weights are panned by the same VBAP the voices use, which gives 7.1.4 its heights. It is normalised so a plane wave has a panned voice's power, and the matrix follows the head each block.

**Budget:**
- The Balanced (Medium) preset:
  - 8 voice slots;
  - 2048 rays × 16 bounces;
  - 1.0 s responses at order 2;
  - 10 Hz.
- Each run simulates the listener's slot and at most half the voice slots (at least 4), new voices first, then round-robin. That halves both the run and the number of responses changing on the render thread at once.
- The simulation thread rests at least as long as each run took, so whatever the quality it keeps its threads busy at most half the time.

**The scene:**
- Direct and reflection simulations share it under a writer-preferring shared lock (`SceneLock`), so they trace concurrently and scene edits aren't starved.

**Open sky** needs nothing special:
- The mesher leaves out surfaces towards unloaded chunks, and the sky has none.
- So rays leaving the loaded region are lost, which is fully absorptive.
- An open field reverberates about 13 dB less than a small room (`test_reflections.cpp`).

## Consequences

- **The phase's golden test.** The simulated RT60 in closed stone rooms is:
  - 0.40 s against Eyring's 0.43 s (small room);
  - 1.08 s against 1.08 s (large room);
  - 2.29 s against 2.43 s (cave).

  The rendered output decays at the simulated time: 2.43 s against 2.32 s in the cave. In the small room the early reflections dominate the first 20 dB (0.28 s against 0.40 s), as expected with a listener-centred source.
- **The CPU budget** (Release, 12-thread desktop, 32 voices of which 8 have reflections):
  - Render thread p99: 17% of the block period on headphones, 13% on 7.1.4. The budget is 25%.
  - A simulation run: about 27 ms on two threads, well under a core. The budget is 1.5 cores.
  - The render path allocates nothing (tested with slot churn, mode switches and a 7.1.4 reopen).
- **Presets.** Low, Medium, High and Ultra trade voice slots, response length and order more than rays (`Config/ReflectionPresets.cs`). The engine's defaults are Medium.
- **Cost of owning the tail.** It is ours to maintain: RT60 accuracy per band is within 15% (the high band's first-order shelf is within 30%), and its level depends on a fitted calibration that should be re-measured if Steam Audio is upgraded.
- **Vanilla's reverb is retired.** `SetReverb` is recorded but unused. With reflections switched off there is no reverb at all; PLAN's parametric fallback bus is not built.

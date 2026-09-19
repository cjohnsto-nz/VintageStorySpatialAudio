# ADR 0012: Every sound is simulated from its place; no other path

**Status:** Accepted, 19 Sep 2026. Supersedes ADRs 0010 and 0011; amends ADR 0009.

## Context

ADR 0011's single listener reverb was stable but wrong in two ways Chris heard at once:
- **No direction.** A reverb simulated for a source at the listener's head reflects off the walls around the listener, not off the walls around the sound. An anvil to the left no longer had its early reflections come from the left.
- **Leaks.** With no simulation of where a sound is, how much of it reached the listener's space was estimated (the direct path, or the way round through the air). The estimate charged nothing for corners and doorways, and nothing rendered the sound arriving by them, so sounds in the next room excited the listener's reverb with nothing audible to explain it.

Steam Audio's per-source simulation has neither problem: its rays find the paths from the listener to the source, so reflections come from their real directions, round corners, through doorways, and not through walls. It never failed. What failed (ADRs 0009 and 0010) was the routing around it: sounds whose simulation was not available fell back to another, much weaker path, chosen by slot bookkeeping, which is why the same anvil rang differently from hit to hit.

Steam Audio's output itself is stable: identical from run to run in a static room, measured to the decibel.

## Decision

- **Every world sound is simulated by Steam Audio from where it is.** There is no other path for its reflections. Head-locked sounds use a source at the listener.
- **Places.** Sounds within 3 m of one another share a place: one simulated source, kept while sounds keep happening there (its simulation refines across runs), let go after 30 s without one. A repeated sound at one spot (an anvil) always uses the same, converged simulation. A sound alone at its place takes the place with it when it moves.
- **When every place is taken:** a new sound takes the place unused the longest; failing that, a place in use whose loudest sound (over the last whole block) is 6 dB below it; failing that, it has no reflections. The sounds of a place taken over lose their reflections until they find another.
- **A new place's first result at once.** The simulation thread polls every 4 ms for places that have never been simulated and runs a tick for those alone, so the first result arrives in about 10–20 ms. Regular runs cover the listener's source and a quarter of the places round-robin, at the configured rate, resting at least as long as each run took.
- **The onset is kept.** A place's effects run from its first input, muted until its first result: the convolution's input history and the tail's pre-delay then hold the sound's onset, so the reflections of a first strike at a new place are heard from the first result on. Only the earliest ones (before that result) are lost, on the first strike only.
- **A slot taken over is reset at once** (its ringing tail is cut; takers are the longest idle or much louder). A stale impulse response from the slot's earlier place may still be read once, muted; a response is committed before its generation is published, so once the new place's results are seen no older one can follow. The `observed` handshake of ADR 0009 is unnecessary and gone.
- **Sends are scaled by `clamp(distance, 1, min_distance)`** (Steam Audio's source is 1 at 1 m; ours is 1 within its reference distance), so reflections keep the direct sound's ratio for any reference distance.
- **Gone:** the listener reverb for world sounds, spots, the air-path estimate (`AirField`) and `vsa_source_debug.air_path` (ABI v10), and `VoiceReflections`. `reflection_sources` is the number of places.

## Consequences

- **Consistency:** six strikes at one spot give the same reflections every time, the first included: -16 dB in the open, -19 dB behind a stone pillar (`test_reflections.cpp`). The pillar case is Steam Audio finding the way round.
- **Walls:** a sound outside a closed stone room adds nothing (-120 dB); inside, its reflections.
- **Decay:** the rendered reverb decays at the simulated time (0.38 s against 0.40 s in a small room, 2.26 s against 2.29 s in a cave).
- **Budget.** Each live place costs the render thread about 55 µs per block at order 2 (its convolution and 16-line tail). Medium is 10 places: with 32 voices at 10 places the render thread is at 18–21 % p99 (budget 25 %; 12 places measured 22–28 % depending on machine load, 16 places 30 %), and a run (a quarter of the places plus the listener) takes about 45 ms on two threads. The next optimisation is the per-place cost: a tail shared by places in one space, or a lighter tail.
- **First strike at a new place:** its earliest reflections (the first 10–20 ms) are lost. Every later strike there has them all.
- **The tail's smoothing** (ADR 0010) must not prime on a slot's empty parameters, or a new place's tail creeps up from silence over a second; it now waits for the first real result, with the onset held in the pre-delay.

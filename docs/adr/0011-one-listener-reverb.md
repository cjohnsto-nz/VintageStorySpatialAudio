# ADR 0011: One listener reverb that every sound feeds by what reaches the listener

**Status:** Superseded by ADR 0012 (19 Sep 2026). Was: accepted, 19 Sep 2026, superseding ADR 0010.

## Context

ADRs 0009 and 0010 gave sounds their reverb through three kinds of slot:
- a voice's own simulated source;
- a "spot" shared by short sounds near it;
- the listener's reverb, stripped of its early reflections.

Each carried very different energy. In a small room about 97% of the reflected energy is early reflections, so the listener's tail-only path was about 15 dB quieter than a spot.

Which slot a sound got depended on bookkeeping, not acoustics:
- whether a spot existed yet;
- whether looping sounds held every slot, since spots were only granted from free ones;
- whether a louder voice had taken the spot away.

So the same anvil struck twice in a small room gave "almost none" one time and "cavernous" the next.

Steam Audio was not the cause. Measured in a static small room, its output is identical from run to run to the decibel, even when scene edits restart its averaging. So the smoothing, position holding and extra rays added for "noise" addressed a problem that did not exist (they stay; they are harmless).

## Decision

- **The listener's reverb is the reverb, complete.** PLAN §5.4's "listener reverb, always on" is Steam Audio's source-at-the-listener simulation, early reflections convolved and our diffuse tail, one path that every world sound takes.
- **Each sound feeds it by the energy that reaches the listener's space**, the louder of two paths:
  - **through what is in the way:** the direct path's gain, `distance gain × (occlusion + (1 − occlusion) · transmission)`, which is PLAN §5.4's "weighted by their direct-path energy";
  - **around it through the air:** `min(1, min_distance / air path)`. The air path is the shortest route through open cells from the listener (`world::AirField`: Dijkstra over the voxels with chamfer steps, 24 blocks each way; doors and fences passable at a 4 m penalty; solids and liquids not; no squeezing diagonally between blocks). It is recomputed on the direct simulation's thread when the listener changes block, or the scene changed and half a second has passed: 9 ms at worst.

  A stone pillar between you and an anvil costs its reverb a little (the way round is longer). A sealed room next door gives only what passes through its walls. Every strike is fed the same way, the first as the rest.
- **Spots are gone.**
- **Voices' own reflections become optional** (`reflection_sources`, 0 by default, which is ABI v9; `VoiceReflections` in the config). When on, the loudest lasting sounds are simulated from where they are, as before (ADR 0009's slot pool, reuse handshake and round-robin).
- **Kept from 0009/0010:**
  - the reference-distance scaling of voices' own reflections;
  - the tail smoothing and onset routing;
  - the early/tail mix controls;
  - the materials.

## Consequences

- **Consistency:** the same sound at the same place always excites the same reverb. Tested with six strikes in the open (all -30 dB) and behind a pillar (all -32 dB), the first as the rest.
- **Walls:** a sound outside a closed stone room adds nothing (-120 dB).
- **Level:** reverb keeps the direct sound's ratio for any reference distance, both through the listener's reverb and through voices' own reflections.
- **Where the reverb comes from:** every sound rings in the listener's space, the physical picture for sound that reaches you. A far sound's own space (a cave heard from outside) is only heard that way with voices' own reflections on, and only for lasting sounds.
- **Debugging:** the air path is in `vsa_source_debug.air_path` and the HUD ("round through the air 9.3 m", "no way round through the air").

# ADR 0010: Reverb follows what reaches the listener; short sounds share spots

**Status:** Accepted, 19 Sep 2026. Amends ADR 0009.

## Context

Chris's first test of Phase 6 found four problems:
- The reverb was far too strong, even at a reflection gain of 0.1.
- Early reflections phased against the game's sounds.
- A wooden house rang like a cave.
- Animals hundreds of blocks away reverberated in the listener's room, as if through walls.

The causes:

- **The shared reverb ignored walls.** ADR 0009 fed every sound without a slot of its own into the listener's reverb at `min_distance / sqrt(d)`, a level set by distance alone. Most game sounds are short one-shots (calls, steps, hits), so almost everything went that way. Steam Audio only respects walls for the sources it simulates; this send bypassed it.
- **Every send was scaled by the sound's reference distance.** Vanilla sounds have one of at least 3 m (`sqrt(range) - 2`), so every send carried +10 dB or more, independent of distance. The right factor is the ratio of our direct gain to Steam Audio's at the same distance: `clamp(distance, 1, min_distance)`.
- **The listener's reverb had early reflections.** They are those of a sound at the listener's head, wrong in time for every other sound, and they combed against the direct sound.
- **The materials were too reflective.** Steam Audio's "wood" preset (7% absorbed mid-band) is a bare hardwood panel, and the scene has no furniture. A 7 × 4 × 7 wooden room decayed in about 2 s.

## Decision

- **The listener's reverb is the room around the listener ringing with what reaches it.**
  - A sound feeds it at its direct path's level: distance gain × (occlusion + (1 − occlusion) · transmission), walls included.
  - It has no convolved early part. Its diffuse tail starts 20 ms after the sound.
- **Reflections of a sound's own (or its spot's) are scaled by `clamp(distance, 1, min_distance)`.** They then keep the direct sound's ratio for any reference distance (tested: 9.54 dB for 1 m against 8 m references).
- **Spots.** A short sound (not looping, not streamed, under 0.75 s) looks, when it starts, for a slot simulated within 4 m (a spot or a voice's own) and shares its reflections.
  - If none exists, the loudest such sound of the block asks for a spot at its position, taken from free slots only; lasting voices keep priority and may take a quieter spot's slot.
  - A spot is kept while sounds keep happening there, and let go 20 s after the last one.
  - So an animal's first call shares the listener's reverb (at what reaches the listener), and its next calls ring in its own room: simulated by Steam Audio from where it is, so walls are respected natively.
- **Building materials absorb more:** wood 0.18 / 0.25 / 0.28, brick 0.10 / 0.15 / 0.18, ceramic 0.05 / 0.07 / 0.08. These are effective values for furnished, unsealed rooms. A furnished 7 × 4 × 7 wooden room now decays in about 0.5 s.
- **Rough materials scatter more:** stone 0.4, wood 0.3, soil and gravel 0.5. Flat voxel faces would otherwise be mirrors with flutter echoes between parallel walls.

## Consequences

- A sound outside a closed stone room adds nothing to the listener's reverb (-120 dB, against -32 dB from inside; `test_reflections.cpp`).
- The rendered decay in a small room now matches the simulation (0.38 s against 0.40 s; it was 0.28 s while the listener's early part dominated).
- Short sounds get their own room's reverb from their second sound at a place on. A single sound at a new place has only the listener's reverb.
- The reverb reaches voices' own and spot slots with Steam Audio's absolute time of flight, but the direct sound has no propagation delay. Reflections of far sounds therefore arrive later after the direct sound than they would in air: 58 ms for a sound 20 m away. A physical propagation delay on the direct path would align them and is left for a later decision.

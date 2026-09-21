# ADR 0022: A sound can be given a floor under occlusion

**Status:** Accepted, 21 Sep 2026.

## Context

A wolf's howl has a range of 110, and with `SoundRangeMultiplier` the game starts it out to 660 m.
In the open it is heard at that distance. Over any terrain it is not: the near ground occludes it
and nothing is left.

That is not occlusion being too strong. It is diffraction being absent. A real howl carries over a
ridge by bending over it, and **Steam Audio models diffraction only along baked paths** — its
`IPLDeviationModel` is documented "Only used when simulating pathing", and both occlusion types
(`RAYCAST`, `VOLUMETRIC`) are visibility tests with no bending in them at all. So the mechanism
that should carry the howl exists in the library, but only inside the probe network.

The probe network is a 64 m box (ADR 0015). At 300 m the source is far outside it: no probes, no
path, no diffraction. What is left is a visibility test against the terrain in the first 128 m of
the scene, which says "blocked", and the sound vanishes.

Extending the probes is not on. A bake costs about probes^2.2 and the current box is 1058 probes
for 458 ms; a 660 m radius is some 400x the surface area, which extrapolates to days per bake.

So at long range the choice is to accept the silence or to approximate. Two approximations were
considered: tapering occlusion with distance for every sound, and a floor under occlusion for
chosen sounds. The second was chosen because the problem is not uniform — a call is the thing that
should carry, and footsteps from the same animal should not.

## Decision

A voice may be given an **occlusion floor**: the least of it that still reaches the listener
directly, however much is in the way.

- It is a floor under **Steam Audio's occlusion**, the visible fraction of the source — not under
  the output level. `params.occlusion = max(simulated, floor)`. Transmission through whatever
  hides the rest is added on top, so the sound is never below that fraction and is usually above.
- It reaches the engine as `vsa_voice_desc::occlusion_floor` (0..1) and
  `vsa_voice_set_occlusion_floor`. **ABI 17 to 18.**
- The mod maps sounds to floors by asset path in `OcclusionFloorBySound`: `*` matches anything,
  the first match wins so the file's order is the priority, and a sound matching nothing is
  occluded exactly as before. Empty by default — nothing changes until a player asks for it.
- The same floor lifts the voice's ranking level, so a floored sound is not ranked down for walls
  that no longer silence it.

## Consequences

- **This is a fudge, and should be read as one.** A floor is not a propagation model. A sound with
  one is audible through a sealed mountain, because that is literally what it says: the test in
  `test_pathing.cpp` seals a room and shows −78 dB without a floor and 0 dB with 0.25. Anyone
  setting one is trading physical truth for being told the wolf is coming, and should set it for
  the few sounds where that trade is worth it.
- Default empty means the mod ships behaving exactly as it did. Nobody gets leakage they did not
  ask for.
- **The ABI bump forces a native pack re-release** (ADR 0021), the first time that rule has been
  exercised. A 0.1.0 pack with this mod now fails the ABI check, whose message names the pack and
  says to update it.
- It does nothing for the real problem. If long-range propagation is ever modelled properly — a
  coarse far-field probe network, a terrain-profile diffraction estimate — this should be removed
  rather than kept alongside it.
- The per-sound granularity is asset paths, not categories, because `creature/wolf/howl*` and
  `creature/wolf/footsteps/*` are both `Entity` and want opposite answers.

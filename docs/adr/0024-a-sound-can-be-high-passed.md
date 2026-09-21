# ADR 0024: A sound can be high-passed, and footsteps are by default

**Status:** Accepted, 21 Sep 2026.

## Context

Creature footsteps boom. The game's own (the wolf's) and the ones the Creature Footsteps mod adds
for nearly everything else were recorded close up, with more low end than a paw on soil has. The
engine plays what it is given, and physics then makes it worse: the low band is what passes
through walls best and what the reverb holds longest, so at any distance the thump is what is left
of the step.

This is the recording, not the propagation, so nothing in the simulation is the place to fix it.
It needs an equaliser on those sounds.

It belongs in this mod and not in Spatial Audio Effects. That mod decides *where* sounds are and
plays them through the game's sound API, which has a low-pass and nothing else; it could only ship
filtered copies of other people's recordings, which misses every footstep a mod adds later and
cannot be tuned. *How* a sound reaches the listener is this mod's half of the division, and the
engine already runs a filter per voice.

## Decision

- A voice may be given a **high-pass corner**: `vsa_voice_desc::high_pass_hz` and
  `vsa_voice_set_high_pass`, 0 for none, 20..2000 Hz. A second-order Butterworth (12 dB per octave,
  `dsp/high_pass.hpp`), coefficients computed when it is set, one state per channel like the
  shelf. **ABI 18 to 19.** The field takes the four bytes of padding at the end of the
  description, so the struct's size is unchanged.
- It runs **before the sends**: the reverb and the paths get the filtered sound too. Filtering only
  the direct sound would leave the boom in exactly the parts that carry it furthest.
- It applies **wherever the sound plays from**: positioned, head-locked or unpositioned.
- The mod maps sounds to corners by asset path in **`HighPassHzBySound`**, with the rules of
  `OcclusionFloorBySound` (ADR 0022): `*` matches anything, the first match wins so the file's
  order is the priority, a sound matching nothing is played as recorded. A corner of 0 exempts
  what it matches from the rules after it.
- **The default is `"*step*": 150`.** Every footstep recording in the game and in Creature
  Footsteps has "step" in its name (`footstep-wolf-dirt`, `drifterstep`, `golem-footstep`), and
  the player's own (`walk/grass`) do not, which is right: those are head-locked and not the
  complaint. 150 Hz takes the thump and leaves the step.

## Consequences

- A sound that happens to have "step" in its name and should boom gets thinner; the fix is a rule
  above the default with a corner of 0. None is known.
- A settings file written before this has no `HighPassHzBySound` and takes the default; one with
  the key, even empty, is left as the player wrote it.

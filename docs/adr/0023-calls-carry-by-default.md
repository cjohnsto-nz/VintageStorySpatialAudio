# ADR 0023: Calls carry by default

**Status:** Accepted, 21 Sep 2026. Amends ADR 0022.

## Context

ADR 0022 gave a sound a floor under occlusion and shipped the table empty, on the reasoning that
nobody should get leakage they had not asked for. Tried in a world, that reasoning does not
survive contact: the setting was deployed and had no effect, because an empty table is a feature
nobody will ever find. The mod's whole claim is that sound behaves as it should, and a wolf that
is silent the moment a hill is in the way is the most noticeable place it does not.

The creatures that suffer are the ones whose calls exist precisely to be heard a long way off.
A wolf's howl has a range of 110 and an elk's bugle 120, against 32 for an ordinary sound: the
game is already saying these carry, and the near terrain was overruling it.

## Decision

Three patterns ship in `OcclusionFloorBySound`, all at **0.25**:

| Pattern | Creatures |
|---|---|
| `creature/wolf/howl*` | wolf |
| `creature/animal/mammal/hooved/deer/elk-bugle*` | elk (the call its AI makes) |
| `creature/animal/mammal/hooved/deer/elk-bellow*` | elk and caribou (`deer-caribou-*` has no sounds of its own and reuses the elk bellow) |

- **Calls only.** `elk-hurt*`, `elk-death*` and caribou's `redstag/roar*` are left alone: a
  wounded animal carrying through a mountain is not the effect wanted.
- **Existing files are migrated.** `CurrentConfigVersion` goes to 2, and a file below it whose
  table is empty gets the defaults. A table a player has filled in is theirs and is untouched.
- **An emptied table sticks**, once the file is at version 2. `OcclusionFloorBySound` is marked
  `ObjectCreationHandling.Replace`, without which Newtonsoft merges the file into the property's
  default and an emptied table would come back on every load; a test pins that.

## Consequences

- Out of the box, a wolf behind a ridge is heard. That is the point, and it is the behaviour most
  likely to be described as the mod working.
- **A dying moose carries through terrain**, because `moose-adult.json` uses `elk-bugle*` as its
  death sound. Keeping it out would need the floor keyed on something finer than the asset path,
  which it is not. The noise is small; the alternative is dropping elk calls.
- 0.25 is a first number, tuned by arithmetic rather than by ear: with a reference distance of
  about 17 m at `ReferenceDistanceMultiplier` 2, it puts a howl at roughly −27 dB at 100 m and
  −37 dB at 300 m. It has not been listened to at close range, where the same floor means a howl
  from just behind a boulder is only −12 dB down, which may prove too leaky.
- ADR 0022's reasoning stands for everything else: the table is still the player's, and no sound
  not named here is touched.

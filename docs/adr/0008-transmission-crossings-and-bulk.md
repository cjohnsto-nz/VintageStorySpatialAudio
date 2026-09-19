# ADR 0008: Transmission is a loss per crossing plus a loss per metre

**Status:** Accepted, 19 Sep 2026. Amends ADR 0004.

## Context

ADR 0004 computes transmission from the path length inside each material: `T = Π exp(−α · length)`. Bulk loss alone makes thin things nearly transparent. A closed door (a 1/8-block box) or a glass pane would cost a fraction of a dB. Real partitions lose most of their attenuation at the surfaces (the mass law), not in proportion to thickness.

## Decision

- **Each material has two losses per band:**
  - a *crossing* loss, applied once each time the listener–source line enters the material;
  - a *bulk* loss in dB per metre inside it.
- **How the walk accounts for them:** it goes through the voxel grid cell by cell. A run of cells of one material is one crossing. Each partial block's box the line passes through is one crossing plus its length.
- **Where the values live:** `vsa_acoustic_material.transmission` carries the crossing loss (as an amplitude per band, the same meaning Steam Audio gives surface transmission) and `attenuation_db_per_metre` the bulk loss. The ABI is unchanged; only the meaning is fixed.
- **Blocked centre line:** the result multiplies the occluded part of the source: `gain = occlusion + (1 − occlusion) · T`. Occlusion is Steam Audio's volumetric value.
- **Clear centre line, partly occluded source** (a corner): the hidden part takes a fixed edge loss (6/9/12 dB), so moving past a corner is gradual.
- **Sources inside a solid block** (block sounds are placed at block centres) are moved out of it, towards the listener, by up to two cells before simulation.

## Consequences

- The shipped values give glass < wood < 1 stone < 3 stone < 6 stone in every band (tested in voxels, and end to end through Steam Audio's direct effect).
- Steam Audio's 3-band transmission EQ limits the deepest losses (about 85 dB); thick rock is effectively silent anyway.

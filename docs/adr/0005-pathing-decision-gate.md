# ADR 0005: Pathing decision gate

**Status:** Accepted, 19 Sep 2026

## Context

Steam Audio's pathing (sound around corners and through doorways) needs baked probe-to-probe visibility (`iplPathBakerBake`). The prototype never baked, so its pathing was always silent. Baking assumes mostly static levels, while ours are procedural and editable.

## Decision

Phase 7 starts with a measured spike. We bake a 64 × 64 × 64 region with typical caves and buildings on one worker thread.

- **Bake ≤ 2 s:** ship baked pathing.
  - Regional probe batches are generated with `IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR` and baked in the background.
  - `enableValidation` and `findAlternatePaths` keep paths correct when blocks are edited between bakes.
  - A dirty region re-bakes with hysteresis.
- **Otherwise:** voxel portal propagation.
  - A flood fill from the source over the air/transmission grid finds the dominant arrival direction and path length.
  - That is converted to spherical-harmonic coefficients and EQ, and fed to the same `IPLPathEffect`.

## Consequences

- The renderer, the tests and the "goat in a room, doorway to a hallway" acceptance test are the same either way.
- The decision is data-driven and recorded in a follow-up ADR.

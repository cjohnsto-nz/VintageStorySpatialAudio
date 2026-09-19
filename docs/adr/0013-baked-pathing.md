# ADR 0013: Pathing is baked (the ADR 0005 gate, measured)

**Status:** Accepted, 19 Sep 2026. Decides ADR 0005.

## Context

ADR 0005 made Phase 7's first task a measurement: bake Steam Audio pathing data for a 64 × 64 × 64 region of typical world on one worker thread, and ship baked pathing if that takes 2 s or less; otherwise build voxel portal propagation.

## Measurement

`native/tests/core/test_pathing_bake.cpp` builds a region of bumpy terrain with a wooden house and a stone hall (doorways) and a winding cave down to a chamber (143 k air cells, 8.3 k triangles in 8 chunks), generates probes 2.5 m apart and 1.6 m above every floor, and bakes with `iplPathBakerBake` (radius 1 m, threshold 0.1, visibility range 32 m, path range 64 m). Release, one thread, on the development machine:

| | |
|---|---|
| Probes | 794 (generated in 1.2 ms) |
| Bake, 1 visibility sample per probe | **0.52 s** |
| Bake, 4 visibility samples per probe | 2.1 s |
| Pathing data | 2.6 MB per region |

## Decision

- **Baked pathing ships.** Regions bake in the background per ADR 0005: `IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR`, several region batches active at once, `enableValidation` and `findAlternatePaths` covering edits between bakes, a dirty region re-baking with hysteresis.
- **One visibility sample per probe** by default (four costs 4× for little: the data is the same size and the paths the same in this world). A quality setting can raise it.
- **The test stays** as the bake's budget (2 s in Release).

## Consequences

- Memory: 2.6 MB per 64³ region; a 5 × 5 × 3 region neighbourhood is ~200 MB, over half of PLAN's 300 MB native budget. The active set must be smaller than that or path range shorter; to be decided when the region manager is built (fewer vertical regions first: most probes sit on the surface and on cave floors).
- **Steam Audio 4.8.1 needs a progress callback.** `iplPathBakerBake` documents it as optional, but `BakedPathData`'s constructor calls it unconditionally; a null callback crashes. Always pass one (a no-op).
- The probe box is centred on the transform's translation: Steam Audio's "unit cube" for probe generation spans −0.5..0.5, whatever the header says.

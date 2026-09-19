# ADR 0014: Pathing runs in one probe batch that rolls with the listener

**Status:** Accepted, 19 Sep 2026. Amends PLAN §5.5 and ADR 0013.

## Context

PLAN §5.5 and ADR 0013 planned probe batches per 64 × 64 × 64 region, several active at once, with a region manager to bake, evict and re-bake them. Steam Audio 4.8.1 finds paths within **one** probe batch only: a path is a walk over one batch's probe graph from the probes the source sees to the probes the listener sees, and no batch knows another's probes. Per-region batches would give no path across a region border, so a doorway on one would be silent. That rules out the region set as planned.

The bake is cheap (ADR 0013: 0.5 s per 64³ region on one thread), so one larger batch round the listener, baked again as the listener moves on, costs little and has no borders where it matters.

## Decision

- **One probe batch, a box round the listener:** `pathing_range` × `pathing_height` × `pathing_range` blocks (default 96 × 64 × 96), its centre snapped to 8 blocks. Probes are `UNIFORMFLOOR` (2.5 m apart, 1.6 m above every floor); the bake uses a radius of 1 m, a threshold of 0.1, a visibility range of a third of the range and a path range of the range.
- **Baked again** (`PathBaker`, on its own thread) when the snapped box centre is more than a third of the box from the current one, when the floating origin moves, or when a chunk inside the box has changed and 3 s have passed without another change. The bake reads a snapshot of the chunks (`WorldScene::Snapshot`) so edits and the other simulations are never held up.
- **Its own simulator** (`PathSimulator`, PATHING flag, `maxOrder` 1, 10 Hz): each run swaps in the baker's latest batch if it changed (added and committed; the old one released after), sets the `pathing_sources` (default 16) loudest sounds that want a path, runs with `enableValidation` and `findAlternatePaths` so paths stay right between bakes, and publishes each sound's order-1 coefficients and EQ through a lock-free channel. A sound wants a path when its direct simulation reports occlusion below 0.9 (or when there is no direct simulation).
- **Rendering:** one `IPLPathEffect` per effect set (order 1, not spatialised) into an order-1 world-space Ambisonic path bus, decoded like the reflections (binaural on headphones, the AllRAD speaker decoder otherwise). The coefficients carry the path length's 1/d and the deviation EQ; the send is the voice after gains and fades, scaled like the reflections' (ADR 0012).
- **Debugging:** `vsa_engine_get_pathing_stats` (bakes, probes, box, ticks, wanted / simulated / found) and `vsa_engine_get_path_segments` (the legs Steam Audio considered in the last run, from its visualisation callback), shown by the `Paths` overlay and the HUD.

## Consequences

- **The goat is heard from the doorway.** In `test_pathing.cpp` a sound inside a stone room, ahead-left of a listener outside, arrives from ahead (the doorway) with paths: the 7.1.4 energy leans 0.60 ahead and 0.00 sideways, against 0.30 to the left through the wall without them, and 79 dB louder. A sealed room finds no path; a sound in the open wants none.
- **Cost:** the render path allocates nothing with paths in play (tested); 32 blocked voices with 16 paths render at a p99 of 7 % of the block on headphones and 5 % on 7.1.4 (Release, `test_render_budget.cpp`); a simulation run is 0.1 ms. Memory is one batch: about 2.6 MB per 64³ of it.
- **Nothing beyond the box.** A sound farther than the range has no path; the direct sound and reflections still reach. Larger ranges cost bake time (linear in probes) and memory; Ultra can raise it.
- **A new box has no paths until its bake lands** (about half a second at the default range): sounds keep their direct sound and reflections meanwhile. A bake is not cancelled when the listener moves on again; the next box follows it.
- **Steam Audio 4.8.1 gotchas** (with ADR 0013's):
  - A simulator's `maxOrder` sizes the pathing coefficients, and a run writes `pathingOrder`'s worth regardless. With `maxOrder` 0 the three directional coefficients are lost (the paths were omnidirectional until this was found); `maxOrder` must be at least the pathing order.
  - A source that finds no path keeps its last. When no probe is in reach of the source or the listener (a sound beyond the box, or sealed in), `findPaths` returns before it zeroes the coefficients, and the simulator copies the old ones out as this run's result. With one source per effect set, a villager underground 80 blocks away took over the set of a sound that had a path through a doorway and played through it, at that sound's level. The simulator therefore **creates a fresh source for every run** (pathing-only sources hold four coefficients and an EQ; the churn costs nothing measurable) and releases the old one after the commit that removed it. `test_pathing.cpp` keeps the case.
- **Not done:** a memory cap and a cancellable bake (the box is small enough that neither matters yet), and pathing for head-locked sounds (they have none by definition).

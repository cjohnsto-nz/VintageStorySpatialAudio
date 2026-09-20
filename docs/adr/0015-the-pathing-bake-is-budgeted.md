# ADR 0015: The pathing bake is bounded by a probe budget, and abandoned when the listener moves on

**Status:** Accepted, 20 Sep 2026. Amends ADR 0013 and ADR 0014.

## Context

ADR 0013 measured one bake of a 64 × 64 × 64 region — 794 probes, 0.52 s — and concluded that
baked pathing was affordable. ADR 0014 then shipped a 96 × 64 × 96 box round the listener.
Phase 8's profiling (`docs/investigations/performance.md`) measured what that costs in play:

| probes | bake | where |
|---|---|---|
| 794 | 0.52 s | the ADR 0013 gate region |
| 1941 | 3.4 s | standing in a village |
| 3859 | **16.7 s** | walking over open ground |

Bake time grows as roughly **probes²·²** — it is a probe-to-probe visibility matrix — and how
many probes a box holds is decided by how much open floor is in it, which is terrain, not
configuration. The gate measurement was of one region of one test world, and generalised badly.

Walking, the consequences compound: five bakes in 72 s meant the baker never stopped, Steam
Audio's workers took a whole core, the sampler saw 7.67 cores at peak, and the run logged the
first stream underruns we have seen. Worse, every one of those bakes was already wrong when it
landed — the listener had walked out of the box it was baked for — so pathing was of little use
precisely while moving, which is when sound sources change most.

## Decision

- **A probe budget bounds the bake.** `pathing_max_probes` (default **1200**) is a ceiling on the
  probes in a box. Probes are generated at the configured spacing; if there are more than the
  budget, the spacing is widened by `sqrt(probes / budget)` and they are generated again, at most
  four times. Probes go as 1/spacing², so this converges in one or two tries. The spacing
  actually used is reported (`vsa_pathing_stats.probe_spacing`).
- **The default box shrinks from 96 to 64 blocks across.** It is the ADR 0013 region's size,
  where the gate was measured, and roughly halves the probes before the budget has to act.
- **A bake whose box the listener has left is abandoned.** `set_listener`, called every frame,
  compares the listener with the centre of the box being baked; beyond a third of the box — the
  same threshold that makes a re-bake due — it calls `iplPathBakerCancelBake`, the result is
  discarded, and the next pass starts a bake where the listener is now. Abandoned bakes are
  counted (`vsa_pathing_stats.cancelled_bakes`).

## Consequences

- **The bake is bounded by configuration rather than by terrain.** At the budget, a bake is about
  1.3 s by the probes²·² fit. `test_pathing_bake.cpp` holds the behaviour: the gate region's 794
  probes are untouched by a budget of 1200, and under a budget of 300 they become 266 probes
  4.3 m apart, baked in 34 ms rather than 512 — fifteen times faster for three times fewer
  probes, which is the superlinearity working for us.
- **Coarser paths in complex ground.** Where the budget bites, probes stand further apart, so a
  path may be found round a wider corner than the one that exists. Doorways within the spacing
  are unaffected; this costs precision in cave systems, where probe density was highest. Raising
  `PathingMaxProbes` buys it back at a steeply climbing bake cost, which is the enthusiast's
  choice to make.
- **Shorter path range.** 64 blocks rather than 96, with the visibility range a third of it.
  Sounds further off have no path; the direct sound and reflections still reach them.
- **Moving, pathing keeps up or says nothing.** A bake that would have been stale is abandoned
  early rather than finishing and being thrown away, so the thread is free for the box the
  listener is actually in.
- **ADR 0013's gate measurement stands as a measurement** but not as a budget: one region of one
  world is not the worst case, and a cost that grows superlinearly needs a ceiling, not an
  average. Any future measurement of this kind should be taken at the shipping settings while
  moving, which is how this was found.

## Amendment, 21 Sep 2026: the budget is 300 where there is no Embree

CI's macOS job took 32 minutes against 1.5 on the others, and the whole difference was four
bakes. The same 794-probe bake with the same ray tracer (Steam Audio's built-in one, one thread)
takes 0.5 s on the x64 runners and 430–480 s on the arm64 macOS one; 266 probes take 1.1 s there.
On an Apple Silicon Mac no bake at the usual budget would ever finish before the listener had
walked out of it (the moving-listener test reported "0 bakes"), and a core would be busy for the
whole session. Why Steam Audio's bake is a thousand times slower there is not known; everything
else in the suite, reflections included, runs at the other platforms' speed.

So on a machine with no Embree, which today is exactly Apple Silicon, `pathing_max_probes`
defaults to 300 (`SteamContext::embree_on_this_machine`, asked before any engine exists so that
`vsa_get_default_config`, and with it the settings file, says 300 too). Probes come out about 4 m
apart rather than 2.5: coarser in tight places, but it works. An explicit value still stands. The
bake tests use that budget there, and the two that exist only to time 794-probe bakes are skipped.

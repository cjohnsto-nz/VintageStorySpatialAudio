# Performance: the mod against vanilla

Phase 8 starts with measuring. PLAN §9 sets the budgets (Balanced preset, mid-range desktop):
render thread ≤ 25 % of the block at p99 with no xruns; simulation threads ≤ 1.5 cores in total;
main thread ≤ 0.5 ms per frame on average; native memory ≤ 300 MB excluding decoded assets. This
page holds the method and the numbers.

## Tools

- **`.steamaudio perf`** (in game): since the last `.steamaudio perf reset`, the mod's own
  main-thread time per frame by section (listener, entity tracking, event pump, sound creation
  including asset decoding, the `ILoadedSound` calls, the scene tick and its chunk reads, the
  debug HUD and overlay) with the managed memory each allocated; frame time median / p99 / worst
  and fps; every thread's share of one core over the window, the engine's by name, Steam Audio's
  workers as one row, the game's and the runtime's by module (Vintagestory.exe, coreclr.dll,
  the display driver, and with the mod off, OpenAL); the process's cores; the engine's render
  time, overloads and underruns; the simulations' tick times; memory (working set, private, GC
  heap and allocation rate, scene, assets).
- **`scripts/perf-sample.ps1 -Seconds 60 -Label <name>`** (outside the game): samples the
  process every second (cores used, working set, private bytes, threads) and writes
  `artifacts/perf/<name>.csv` with a summary. It sees vanilla and the mod alike, so it is the
  comparison's common measure.
- **F3** (the game's debug screen) shows the game's own fps in both runs.

## Method

One world, one spot, one activity, twice: the mod **disabled in the mod manager** (vanilla
OpenAL), then **enabled** at the defaults (Medium reflections, pathing on). Sixty seconds each,
after the chunks around have loaded and the frame rate has settled. Pick spots that work the
mod hard:

1. **Village**: villagers, animals, a windmill, doors; many sounds, many blocked.
2. **Cave**: reverb and pathing round bends.
3. **Forest at night**: lots of ambient sources, few walls.

For each spot: `perf-sample.ps1` for both runs, F3 fps for both, `.steamaudio perf reset` then
`.steamaudio perf` after the sixty seconds for the mod run. Note the CPU model and core count.

## What to look at

- **Frame cost**: the mod's main-thread ms per frame (budget 0.5). The sections say where it
  goes; sound creation includes decoding assets the game has not decoded yet (vanilla decodes
  them at the same moment, on the same thread, so that part is not a difference).
- **Cores**: the process's cores with the mod minus without is the mod's whole cost, Steam
  Audio's workers included. The thread rows attribute it: render callback, the three
  simulations, the baker, the scene builder, Steam Audio's ray tracing.
- **Frame time p99**: the stutter measure. A mod that costs cores on other threads should not
  move it on a machine with cores to spare; if it does, the threads are contending with the
  game's (priorities, or too many Steam Audio threads).
- **Memory**: working set with minus without; the scene and the probe batch are the native part,
  assets the decoded sounds (vanilla keeps them too, in OpenAL buffers).

## Results

_To be filled in from Chris's runs._

| Spot | Run | fps (F3) | frame p99 ms | cores | working set MB | mod main thread ms/frame | notes |
|---|---|---|---|---|---|---|---|
| village | vanilla | | | | | – | |
| village | mod | | | | | | |
| cave | vanilla | | | | | – | |
| cave | mod | | | | | | |
| forest, night | vanilla | | | | | – | |
| forest, night | mod | | | | | | |

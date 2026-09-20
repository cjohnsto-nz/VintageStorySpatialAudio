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

One world, one spot, one activity, twice.

**The baseline run** keeps the mod loaded but idle: `"TakeOverGameAudio": false` and
`"BuildWorldScene": false` in `vssteamaudio.json`. Vanilla OpenAL plays every sound, the engine
sits with its threads parked and no device open, and the mod's only work is counting frames — so
`.steamaudio perf` reports the game's own frame times, measured exactly as they are measured in
the other run. Its first line says which run it is. (Disabling the mod outright gives a purer
CPU and memory baseline, but then nothing inside the game measures frames and the comparison
rests on the debug screen's fps: worth one run as a sanity check on the process figures, not the
one to compare frame times against.)

**The mod run** is the defaults: `TakeOverGameAudio` and `BuildWorldScene` true, Medium
reflections, pathing on.

Sixty seconds each, after the chunks around have loaded and the frame rate has settled. Pick
spots that work the mod hard:

1. **Village**: villagers, animals, a windmill, doors; many sounds, many blocked.
2. **Cave**: reverb and pathing round bends.
3. **Forest at night**: lots of ambient sources, few walls.

For each spot, in both runs: `.steamaudio perf reset`, then `perf-sample.ps1` for the sixty
seconds, then `.steamaudio perf`. Note the CPU model and core count. The game's own debug screen
is **Ctrl+F3** (Alt+F3 for the fps graph alone) if you want to watch the frame rate live; plain
F3 is not bound to anything.

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

**Village, 20 Sep 2026** (Chris's machine, 12 logical cores; a heavily modded world at
-14531 122 1292; defaults: Medium reflections, pathing on). Runs four minutes apart.

| Run | fps | frame median / p99 / worst ms | cores of 12 | working set MB | our main thread ms/frame |
|---|---|---|---|---|---|
| baseline (vanilla audio, mod idle) | 76.5 | 12.9 / 23.6 / 65.3 | 1.76 | 5817 | 0.000 |
| mod | 72.0 | 13.6 / 22.1 / 72.0 | 2.02 | 5684 | 0.414 |

### What it costs

- **Frame time: +0.7 ms median** (12.9 → 13.6), which is 76.5 → 72.0 fps, about 6 %. Our own
  main-thread work is **0.414 ms** of that 0.7, so the frame cost is very nearly just the work we
  do on the frame; the rest is within run-to-run noise.
- **Stutter: none added.** p99 22.1 ms with the mod against 23.6 without, worst frame 72 against
  65 — both inside the variation between runs. The background threads are using spare cores, not
  contending with the game's.
- **CPU: +0.5 cores** on our own threads, measured directly: reflection simulation 0.17, audio
  render 0.11, path baker 0.065, Steam Audio's 14 workers 0.15, everything else under 0.01. The
  process total moved less than that (1.76 → 2.02) because the game does proportionally less work
  at 72 fps than at 76.5; the thread rows are the number to trust.
- **Memory:** our native scene is 30.8 MB (405 chunks, 150 k triangles) and decoded assets
  173 MB, well inside PLAN's 300 MB. The process working set differs by less than the
  run-to-run variation, so it says nothing either way.
- **Budgets (PLAN §9):** main thread 0.414 ms against 0.5 — inside, but only just. Simulation
  threads 0.5 cores against 1.5 — comfortable. Render thread 512 µs average of the 5.33 ms block,
  1827 µs worst (34 %), **0 overloads and 0 underruns** — inside the 25 % p99 rule with room.

### Where the frame cost is

| Section | ms/frame | worst call | allocated |
|---|---|---|---|
| scene tick | 0.200 | 15.6 ms | 67 MB |
| — of which chunk reads | 0.194 | **14.1 ms** | 67 MB |
| events, settings | 0.010 | 9.0 ms | 74 MB |
| sound API (71 k calls) | 0.006 | 1.4 ms | 13 MB |
| listener, entity tracking, sound creation | 0.003 | 0.06 ms | 2 MB |

Chunk reading is nearly all of it. The average is small but a single read has hit **14 ms** —
one dropped frame — so the worst case matters more than the mean. We allocate 217 MB over the
71 s window (3 MB/s) against the game's own 209 MB/s, so we are not driving the collector.

### Worth fixing, in order

1. **Chunk reads on the main thread** (0.194 ms/frame, 14 ms worst). The copy has to start on
   the main thread, but the per-read budget is clearly being blown by single large chunks.
2. **The pathing bake: 3.4 s, worst 4.4 s, 1941 probes** for the default 96 × 64 × 96 box —
   against 0.5 s for 794 probes in the ADR 0013 gate measurement, because the bake grows
   superlinearly with probe count. It costs 6.5 % of a core while walking and leaves new ground
   without paths for three seconds. A smaller default box or wider probe spacing would buy most
   of it back.
3. **Reflections at 22.3 ms per tick** (worst 36.2) are the largest single background cost, but
   they are inside their budget and audibly the point of the mod. Leave unless the quality
   presets need rebalancing.

### With more sounds: the surroundweather mod

A third run at the same spot with `surroundweather` also loaded, which roughly doubles the
simulated sounds (38 voices, 11 with effects → 53 voices, 28 with effects):

| | mod | mod + surroundweather |
|---|---|---|
| fps | 72.0 | 73.4 |
| frame median / p99 ms | 13.6 / 22.1 | 13.4 / 22.6 |
| our threads | 50 % of a core | 84 % |
| — Steam Audio workers | 15.3 % | 37.7 % |
| audio render thread | 512 µs avg (34 % worst) | 848 µs (45 % worst) |
| our main thread | 0.414 ms/frame | 0.546 ms/frame |
| decoded assets | 173 MB | 229 MB |

- **More sounds cost background cores, not frame time.** +0.33 cores, +336 µs on the render
  thread, no overloads — and the frame rate did not fall (73.4 against 72.0 is inside the noise).
  That is the design behaving as intended.
- **The main thread going over its 0.5 ms budget is not the extra sounds.** Their contribution
  across the sound API, sound creation, the listener and entity tracking is about +0.008 ms; the
  rest is chunk reading (0.194 → 0.256 ms) in a window that was loading much more world (scene
  builder 0.2 % → 2.5 %, 2469 → 3261 reads).
- Rough scaling for the render thread: about **20 µs per simulated sound**, so 64 of them would
  be near 1.8 ms of the 5.33 ms block.
- The worst reflection tick rose to 54 ms of its 100 ms period with 28 sources.

### Still to measure

The cave and forest spots, and a second run of each configuration. The process-level CPU and
memory figures varied by 40 % between two identical baseline runs an hour apart (1.15 then 1.75
cores), so only the directly-measured numbers — our threads, our main thread, our memory — are
solid from a single pair.

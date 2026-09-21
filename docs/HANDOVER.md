# Handover (19 Sep 2026)

State of the work at the end of the first Claude Code session on Chris's Windows machine. Earlier sessions ran in a cloud sandbox that could only edit files.

## The name (20 Sep 2026)

The mod is **Spatial Audio**: mod id `spatialaudio`, command `.spatialaudio`, settings in
`ModConfig/spatialaudio.json` and `spatialaudio-materials.json`, C# namespace
`VintageStorySpatialAudio`. Before its first release it was "Steam Audio" (`vssteamaudio`,
`.steamaudio`); files written under the old name are carried over once (`Config/LegacyFiles.cs`)
and `deploy.ps1` removes the old zip from the Mods folder. "Steam Audio" now only ever means
Valve's library: `phonon`, `native/src/steam`, `SteamAudioValidation`, `ThreadKind.SteamAudio`,
the ADRs' discussion of it. The native library is still `vsaudio`. The checkout folder and the
GitHub repository are still named VintageStorySteamAudio until Chris renames them; the README and
CI links already use the new repository name (GitHub redirects once it is renamed).

## Release packaging (21 Sep 2026, ADR 0019)

The mod database takes 40 MB and all three platforms are 54 MB, nearly all of it `phonon`. A
release is two zips from the CI artifact `spatialaudio-mod`: `spatialaudio_<v>.zip` (the mod, with
the Windows libraries) and `spatialaudiounix_<v>.zip` (`src/SpatialAudioUnixNatives`: the Linux and
macOS libraries, a code mod that does nothing). **Since ADR 0021 the two versions are independent**:
the pack is rebuilt and re-released only when the libraries change, and the mod loads whatever pack
is installed, with the engine's ABI check deciding whether it fits. The pack's dependency on the mod
is a floor (`NativeLibraryResolver.FirstVersionAcceptingAnyPack`), which `NativePackTests` checks.
Not yet tried in a game on Linux or macOS:
that the game unpacks the pack and the mod finds it is tested only as far as the folder logic.

## Released: v0.1.1 (21 Sep 2026)

The repository's first tagged release: https://github.com/cjohnsto-nz/VintageStorySpatialAudio/releases/tag/v0.1.1
(`spatialaudio_0.1.1.zip` and the unchanged `spatialaudiounix_0.1.0.zip`), built by CI run
35552894886 from `4920bfa`. Only `spatialaudio_0.1.1.zip` goes to the mod database; the pack there
stays at 0.1.0 (ADR 0021). **Still to do by hand:** the pack's mod database description says to
install the same version of both and that Spatial Audio will not load libraries from another
version, which ADR 0021 made untrue.

## Where things stand

### Phase 0 (foundations): complete and verified on Windows

- Native, managed, VsaDoctor (28/28 integration points against the 1.22.7 client) and packaging all pass through `pwsh ./scripts/build.ps1`.
- In-game load confirmed by Chris: `.spatialaudio status` showed engine running with Embree, self-test passed, 28/28, "Audio takeover: ready"; clean shutdown ("engine destroyed").
- Housekeeping done: git repo with a baseline commit; `ci.yml` moved from `.github/workflow/` (singular, ignored by GitHub) to `.github/workflows/`; `scripts/dev-runner.ps1` deleted.

### Phase 1 (engine core): implemented, exit criteria met locally

On branch `phase1-engine-core`. Verified on Windows (MSVC `/W4 /WX`, Debug and Release) and with a clang-cl syntax pass using the Linux warning set (`-Wconversion -Wdouble-promotion …`); not yet built by GCC or on macOS (see next steps).

| Exit criterion | Evidence |
|---|---|
| Golden tests for resampling, looping and fades | `native/tests/core/test_resampler.cpp` (THD+N, passband, alias rejection, DC, block-size independence), `native/tests/test_voices.cpp` (seamless loops with and without resampling, dB-linear fades, declicking) |
| SceneLab renders WAVs | `tools/SceneLab`; `build.ps1` renders `tools/SceneLab/scenarios/*.json` into `artifacts/scenelab/` and fails on unmet expectations; CI does the same on Linux |
| No xruns under a synthetic 256-voice load | `vsaudio_core_tests` "256 voices…" (asserts p99 < block period in Release) and `scenarios/voices-256.json` |

Measured on this machine (Release, 48 kHz, 256-frame blocks = 5.33 ms):

| | THD+N 1 kHz, 44.1→48 k | flat (±0.02 dB) to | alias residue (pitch ×2) |
|---|---|---|---|
| Low (4 zero crossings) | −58 dB | 8 kHz (±0.1 dB) | −49 dB |
| Medium (8, default) | −79 dB | 12 kHz | −74 dB |
| High (16) | −107 dB | 16 kHz | −115 dB |

- 256 voices, vanilla-like pitch spread 0.8–1.2 (`voices-256.json`): p99 **25 %** of the block period, p50 ~21 %.
- 256 voices in the core test (1/8 pitched ×1.8): p50 24 %, p99 28–31 %.
- Render path allocations: zero (`vsaudio_core_tests` replaces global `operator new`; works on MSVC too because the test links the core statically).
- A real device opens in the native tests: "AV Receiver (NVIDIA High Definition Audio)", 48 kHz, 6 ch, 480-frame period.

Not yet verified:

- **In game**: `.spatialaudio devices`, `.spatialaudio play effect/woodswitch` (plays through our engine alongside vanilla OpenAL), `.spatialaudio stats`, `.spatialaudio stop`. The mod was not redeployed after Phase 1; run `pwsh ./deploy.ps1 -StopGame` first.
- **CI** (never pushed): GCC `-Werror` (only clang-cl was used locally), macOS universal build (the SIMD header picks SSE2/NEON per slice), ASan/UBSan over the new code, and whether miniaudio's null backend makes the Linux device test open a device (the test accepts either outcome).

### Other audio mods in the Mods folder

`%APPDATA%\VintagestoryData\Mods` also has `vintagestoryacousticlab_0.1.0.zip` and `vintagestorysurroundsound_1.2.3.zip`. Both may patch the same audio methods. Disable them when testing this mod. Phase 2's takeover should also detect foreign Harmony patches on our targets and refuse to take over (nothing checks for this yet).

## Immediate next steps

1. Deploy (`pwsh ./deploy.ps1 -StopGame`), join a world, try the four test commands above, and listen: `.spatialaudio play` of a short effect, a looping ambience and a music track (music streams). Check `client-main.log` for the "Test output opened" line and any `[native]` warnings.
2. Push to GitHub and get CI green on all three platforms (see "Not yet verified").
3. Merge `phase1-engine-core`, then start Phase 2 (engine takeover, PLAN.md §10).

## Multichannel beds (ADR 0015): on branch `multichannel-beds`, to check in game

For Chris's separate weather mod: its 5.1 rain, wind, hail, rumble and thunder tracks now play through this engine as intended.
- **Decoding:** assets of up to 8 channels decode and stream. Each records which speaker every channel is for: Vorbis order, WAV's speaker mask or default order, and a lone surround pair at 110°.
- **Beds:** an unpositioned sound of more than two channels is a bed.
  - **Speakers:** each channel plays from its speaker via `BedPanner` (VBAP over the real layout). 5.1 on 5.1 is copied through. On 7.1.4, 5.1 surrounds fall between the sides and backs.
  - **Stereo:** a fold-down, with surrounds −3 dB on their side.
  - **LFE:** to the LFE channel, or to the front pair.
  - **Headphones:** a head-locked order-3 Ambisonic bus with its own binaural decoder.
- **Positioned sounds** downmix any asset to mono.
- **Tests:** `core/test_bed_panner.cpp` and `test_beds.cpp`. Beds were added to the render path's no-allocation test. A managed test plays a vanilla-style weather bed.
- **Not tested:** no manual listening test yet (in game or in SceneLab).
- **The weather mod:** `C:\Projects\VintageStorySurroundWeather`, branch `weather-only` of the Surround repo, modid `surroundweather`.
  - It patches only vanilla's OpenAL classes (`AudioOpenAl.GetSoundFormat`, `LoadedSoundNative.createSoundSource`). Those are inert under this mod and pass its foreign-patch check.
  - Its modid isn't in `IncompatibleMods`.

## Entity sound tracking (from PLAN Phase 8, done early; managed only, uncommitted)

Sounds played at a creature or player follow it while they play; vanilla leaves them where they started. No Doppler (Chris doesn't want it). Ported in spirit from VintageStorySurroundSound, with the problems found in its review fixed.

- **Named by the game:** prefixes on the three `ClientMain.PlaySoundAt(…, Entity, …)` overloads put the entity in a thread-static context (restored by a finalizer).
- **Announce and claim:** a prefix on `PlaySoundAtInternal` announces the sound with its exact float position (`EntitySoundTracker.Expect`). `CreateSound` claims it by that position.
  - Announce/claim is needed because the first play of an asset decodes on the thread pool, so the sound is created after the call returns.
  - A postfix withdraws the announcement when the call returned 0; otherwise it expires after 10 s.
  - The claimed sound starts at the entity's current position, then moves once per frame in `OnFrame` (only when it has moved at least 1 cm).
- **Matched by position:** the server sends creature sounds as plain coordinates, even in single player, and those come through the typed `PlaySoundAt` overload.
  - `Entity`-category sounds and `creature/` / `voice/` assets are matched to the nearest `EntityAgent` (not the local player) whose body (upright axis plus radius, from the selection box) is within `EntitySoundMatchDistance` (1 block).
  - Nothing is matched if a second creature is within 0.5 blocks of the best.
  - Only the height offset is kept: the horizontal one is mostly client interpolation lag.
- **The player's own sounds** (armour, eating, tools: named at the local player, or at exactly their feet) are head-locked 0.75 m ahead and 0.25 m below the ears (`OwnBodyAnchor`), like vanilla's own footsteps.
  - **Why:** Chris heard armour and eating from the rear speakers. At mid-body, almost straight below the ears, 7.1.4 VBAP puts most of the sound on the nadir, which is shared by all ear-level speakers, so the rears get it too. The listener tilts with the camera, so looking up makes it worse.
  - **Modelled rear and side share:** 30% of the power looking level, 46% at +20°, 74% at +60°. Head-locked, it's about 2% whatever the pitch.
- **What happens when things end:** a sound whose entity leaves `LoadedEntities` stays where it was. Tracking ends when the sound stops.
- **What isn't tracked:** sounds that entity code loads itself (gait, bees, bells, elevators) are left to their owners, which move them.
- **Config:** `TrackEntitySounds`, `InferEntitySounds`, `EntitySoundMatchDistance`.
- **Stats:** `.spatialaudio stats` has a "Following entities" line.
- **Verification:** VsaDoctor 38/38 against 1.22.7. `EntitySoundTrackerTests` cover announcement, expiry, cancel, context nesting, inference (tall creatures, lag, ambiguity) and following/letting go against the engine. All 111 managed tests passed on a worktree of `f62e42a` plus these changes (the working tree's native code was mid-ABI-9 change).
- **To check in game:** walk past running wolves or chickens, and chase a bear. Calls should come from the animal, not from where it was. Watch the stats line for "matched by position" counts.

## Creatures you cannot see still make footsteps (ADR 0020, 21 Sep 2026)

A wolf behind you ran in silence. Its footsteps hang off the frames of its `Walk`/`Run`/`Canter`
animations (`animationSounds` in `wolf-adult.json`, the bear the same), and
`AnimationManager.OnClientFrame` only advances the animator while
`entity.IsRendered || entity.IsShadowRendered || !entity.Alive` — `IsRendered` being set per frame
by `SystemRenderEntities.OnBeforeRender` from the frustum, the view distance and the chunk. So the
sound existed only while the creature was on screen.

- **The patch:** a prefix on `AnimationManager.OnClientFrame` (`PlatformPatches.AnimationOnClientFrame`
  → `AudioTakeover.OnUnseenAnimationFrame`) advances the animator for the creatures vanilla is
  about to skip, with `CalculateMatrices` off and restored afterwards (the matrices only pose the
  model for the shader). It always falls through to the original.
- **What it does not do:** it skips creatures whose active animations carry no sound with a
  location (nearly all of them: `FurthestAnimationSound`), and any whose furthest sound cannot
  reach the listener at `range × SoundRangeMultiplier` — the distance at which
  `PlaySoundAtInternal` would refuse to start it anyway.
- **Placement is unchanged:** `ShouldPlaySound` plays at the creature's position by plain
  coordinates, and the entity-sound inference above matches it and makes it follow.
- **Deliberately outside the foreign-patch check** (`PatchedMethods`): animation mods patch this
  method, our prefix only adds work, and audio should not stand down over it.
- **Config:** `SoundsFromUnseenCreatures` (default on). **Perf:** the `unseen animation` section of
  `.spatialaudio perf`.
- **Verification:** VsaDoctor 40/40 against 1.22.7; `GameIntegrationTests` asserts the installed
  `OnClientFrame` still reads `IsRendered`/`IsShadowRendered` (if a game update fixes it, that test
  fails and the patch should go). All 144 managed tests pass.
- **To check in game:** stand with your back to a wolf or a bear and listen to it approach; turn
  round and the footsteps should not change. Not yet tried in game.

## Phase 8 (release hardening): in progress on `phase8-hardening`

Phase 7 is merged into `main` (not pushed). Chris asked to start with performance: what the mod costs against vanilla.

- **Thread stats (ABI v12):** `core/thread_stats.*`: every engine thread registers by name as it starts (`ThreadScope`); the device callback, which must not allocate, publishes its id and the engine worker announces it. `vsa_engine_get_thread_stats` reports each registered thread's CPU time and, on Windows, walks the process's other threads, naming each by the module its start address lies in (exactly: `phonon.dll` is Steam Audio's workers, `embree*`/`tbb*` ours, the rest the game's and the runtime's). Two readings apart give shares of a core.
- **`Diagnostics/PerfMonitor`:** the mod's main-thread time by section (listener, entity tracking, event pump, sound creation with the asset decode, the `ILoadedSound` calls, scene tick and chunk reads, HUD, overlay), managed bytes allocated per section, frame time median/p99/worst from the game's per-frame listener update. Nested scopes of a section count once; other threads are ignored.
- **`.spatialaudio perf [reset]`** (`Diagnostics/PerfReporter`): the report over the window; `scripts/perf-sample.ps1` samples the game process from outside for the vanilla baseline. The method and the results table are in `docs/investigations/performance.md`.
- **First numbers (village, 20 Sep 2026), in `docs/investigations/performance.md`:** +0.7 ms of frame time (76.5 -> 72.0 fps), of which 0.414 ms is our own main-thread work (budget 0.5); no added stutter (p99 22.1 ms with, 23.6 without); +0.5 cores on our threads (reflections 0.17, audio render 0.11, path baker 0.065, Steam Audio workers 0.15); render thread 512 us average of the 5.33 ms block, no overloads. The frame cost is almost all chunk reading (0.194 ms/frame, but a single read has hit 14 ms), and the pathing bake takes 3.4 s for the default box against 0.5 s in the ADR 0013 gate measurement.
- **The bake was the problem, and is fixed (ADR 0015, ABI v13):** a probe budget (`pathing_max_probes`, 1200) widens the spacing until the box holds no more, since a bake costs about probes^2.2 and the terrain decides the probe count; the default box drops from 96 to 64 blocks; and a bake whose box the listener has left is abandoned through `iplPathBakerCancelBake` rather than finishing and being discarded. `vsa_pathing_stats` now reports the spacing used and the abandoned count (the old `reserved` fields). Measured in `test_pathing_bake.cpp`: under a budget of 300 the gate region goes from 794 probes / 512 ms to 266 probes 4.3 m apart / 34 ms.
- **Retested walking (20 Sep 2026):** the bake is **458 ms, worst 771** against 16.7 s, at 1058 probes 2.8 m apart (the budget barely had to act: the smaller box did most of it). The baker's thread is down from 16.8 % of a core to 3.1 %, Steam Audio's workers from 105 % to 75 %, ours in total from 1.62 to 1.14 cores, and the stream underruns are gone. Twelve bakes landed in 71 s with one abandoned, so pathing keeps up while walking. Frame cost remains nil (64.2 fps against the baseline's 64.5).
- **The settings file now holds real values (ADR 0018, ABI v14):** every setting is written out in full on load, from `vsa_get_default_config`, which resolves a zeroed config through the same path an engine is built with (so the defaults are not duplicated in C#). `.spatialaudio config` prints the file's path and what is in effect; `.spatialaudio config reset` restores the defaults, which is how a player adopts defaults that changed in a new version. A zero still means "the default", so older files keep working.
- **A crash while walking, found and fixed (ADR 0017):** ADR 0015 had a stale bake abandoned through `iplPathBakerCancelBake`. That call is unusable in Steam Audio 4.8.1 — its thread pool's cancel flag is never cleared, so the workers stop waiting and spin calling `processNextJob` on a `JobGraph` the bake has already destroyed; and the pool itself is a global raw pointer to `bake`'s own stack frame. Either way it is silent memory corruption, and the game died seconds later with nothing in `client-crash.log`. We now never cancel: the bake runs to the end and its result is thrown away. `test_pathing_bake.cpp` reproduces the crash (it died within seconds under the old code) and is the case to run under the Linux sanitizers.
- **The honest cost, walking, back to back (20 Sep 2026):** **11 % of the frame rate** (77.9 -> 69.4 fps) and **1.3 cores**. Our own main-thread work is 0.167 ms of the 1.0 ms of frame time; the rest is contention. Frame p99 is 3.7 ms worse. No underruns, no crash, bakes 463 ms.
- **The reflections are rate-bound, not count-bound:** ADR 0016 cut places from 10 to 3 but the tick only went 24.2 -> 21.2 ms, because the simulator runs the listener's slot plus a quarter of the places (3 sources -> 2). The runs fit about 6 ms fixed per tick plus 7-8 ms per source at 4096 rays x 16 bounces. 21.2 ms on three threads at 10 Hz is 0.64 of a core. **Next thing to try: `ReflectionRateHz` 5 instead of 10** -- the renderer smooths over 0.7-1 s, so it should be inaudible and should halve the cost. No rebuild needed to test it.
- **The reflections were next, and are addressed (ADR 0016):** 24.2 ms per tick on three threads was most of Steam Audio's 75 %, with 10 places live for 2 audible sounds because a place is held 30 s after its last sound (ADR 0012). An idle place more than 48 m from the listener is now let go at once, so walking no longer leaves a trail of simulated ground; within 48 m the hold stands, which is the anvil case ADR 0012 wanted. To be measured on the next walking pair.
- **The sound inspector (ABI v15), `.spatialaudio scene sounds [page|off]`:** every sounding voice, **loudest first**, with what it is, its bus, its distance, and **which way it is reaching the listener** — in the clear, through walls, round a corner, or only its reflections — with the level each way carries, plus the direct simulation's occlusion, metres of material and per-band transmission. A page at a time (8 a page), because the HUD cuts off what it cannot fit. The render thread works the numbers out per voice per block only while the inspector is on (`vsa_engine_set_inspect`), so it costs nothing otherwise. The levels are the amplitude each way carries **into** its effect, not a measurement of what comes out; they are for comparing one way against another.
- **Debugging one sound, one way at a time (ABI v17):** `.spatialaudio solo anvil` silences every sound whose name does not contain the text (`vsa_voice_set_muted`, a per-voice mute in the engine that gains and fades cannot undo; the game thinks they still play), turns the inspector on and draws the path legs. A muted voice asks for no path, so **with a sound soloed every leg the paths overlay draws is that sound's** -- which is how the legs are attributed per sound without Steam Audio saying whose they are. `.spatialaudio mute direct|paths|reflections|off` toggles a gain of 0 on one way (`vsa_engine_set_route_gains`, and the reflection gain), to hear which way a leak arrives by. The inspector's heading shows what is soloed and muted.
- **How far sounds carry is now a setting.** The fall-off itself is physical and stays that way (`Mixer::spatial_params`: full volume within a sound's reference distance, `reference / distance` beyond it, plus Steam Audio's air absorption per band). `ReferenceDistanceMultiplier` (0.25 to 8, default 1) scales every sound's reference distance, which moves the whole curve outwards rather than bending it: 2 is about 6 dB more at every distance past the reference. `SoundRangeMultiplier` (default 3) is a different thing -- it patches the game's own range check so distant sounds are started at all -- and wants raising alongside.
- **The inspector is a live panel and a world overlay (ABI v16):** `.spatialaudio scene sounds` fills the HUD with what is sounding, loudest first, and draws each one where it is, coloured by how it reaches you -- **green** in the clear, **orange** through walls, **blue** round a corner, **magenta** only its reflections. A sound arriving round a corner also gets an **arrow from you to where it really comes in**, which is the answer to "is the mod broken, or am I hearing reflections?": the arrow points at the doorway, not at the sound. The direction comes from the first-order part of the path field -- Steam Audio projects a direction (x, y, z) into the Google SH library's frame as (-z, -x, y), and that library's order-1 coefficients are its (Y, Z, X), so the scene direction is (-sh[1], sh[2], -sh[3]). `test_pathing.cpp` pins it: the goat is ahead-left behind a wall and the arrival reads (0, 0, -1), the doorway.
- **The HUD sizes itself** to what it is given, up to the window height (`SceneHud.LineBudget`), and the inspector's panel shows the sounds alone, a page at a time, so nothing it needs is pushed off the bottom. It was a fixed 720x300 box that silently cut off the rest.
- **Still to do for the debug views:** a path's actual legs and a reflection's rays are still drawn for every sound at once, not per sound -- Steam Audio reports them for a whole run, with nothing to say which source each belongs to. The arrival arrow covers the common case; per-sound legs would need the pathing run split per source.
- **Chiselled blocks had no geometry (20 Sep 2026):** `Block.CollisionBoxes` defaults to a full unit cube, and the classifier returned `Full` on that *before* it checked for a block entity — so every chiselled block was acoustically a solid cube, and its material was the microblock type's rather than what it is made of. Now anything with a block entity is read per snapshot (its entity's boxes are the truth: a crate's give a cube back, a chiselled block's give its cuboids), and the material comes from `GetBlockMaterial(accessor, pos)`, which for a microblock is the majority material of its voxels. Boxes per block are capped at 32, largest first (`BlockClassifier.MaxBoxes`), since each is 12 triangles and a test in every ray through the cell. **Costs a block-entity lookup per such cell per chunk read** — worth watching in the perf report's chunk-read line, which was 0.067 ms/frame with 4 calls over 2 ms before.
- **The reflection budget test is flaky under machine load** (three failures today, all passing standalone): with the machine busy its p50 nearly doubles (12 % -> 20 %) and the scene-edit test's worst goes 3 ms -> 77 ms. It already takes the best of five windows. Worth making load-aware before CI depends on it.
- **Known before the 3-OS build:** `clang-check` over the whole tree reports **26 `-Wdouble-promotion` warnings in the test files** (`doctest::Approx(someFloat)` promotes to double). They are warnings-as-errors on Linux, so they must be cleared before that CI is turned on. No production source has any.
- **Next:** Chris re-runs the walking pair with these defaults, then the remaining spots (cave, forest) and fills the table; then whatever the numbers say (thread priorities, Steam Audio thread count, main-thread hot spots), then the rest of Phase 8: entity-bound source tracking review, compatibility modes, presets, docs, packaging, the 2-hour soak.

## Phase 7 (pathing): done, merged

Phase 6 is merged into `main` (not pushed).

- **The gate (ADR 0013):** a 64³ region of terrain, two buildings and a cave bakes in 0.52 s on one thread with one visibility sample per probe (2.1 s with four): 794 probes, 2.6 MB. Baked pathing ships. `core/test_pathing_bake.cpp` keeps the budget.
- **Two Steam Audio 4.8.1 gotchas found:** `iplPathBakerBake` crashes without a progress callback (pass a no-op), and the probe generation box is centred on the transform's translation (its unit cube is −0.5..0.5).
- **One rolling probe batch (ADR 0014), not per-region batches:** Steam Audio finds paths within one batch only, so a region set would have had silent doorways on region borders. The batch is a 96 × 64 × 96 box round the listener (`pathing_range`, `pathing_height`), snapped to 8 blocks, baked on the baker's thread from a `WorldScene::Snapshot`, and baked again when the listener leaves the middle third, the origin moves, or a chunk in it has changed and 3 s have passed without another change.
- **Third gotcha:** the simulator's `maxOrder` sizes the pathing coefficients and a run writes `pathingOrder`'s worth regardless; with `maxOrder` 0 the paths came out omnidirectional. `PathSimulator` sets `maxOrder` 1.
- **Fourth gotcha (Chris heard villagers underground 80 blocks away as if beside him):** a Steam Audio source that finds no path (no probe in reach of the source or the listener) keeps its last coefficients, and they come out as the run's result. A far sound taking over an effect set played through the path of the sound before it. `PathSimulator` now creates a fresh source every run (ADR 0014 has the detail); `test_pathing.cpp` keeps the case (a sealed sound 60 blocks away: -217 dB, was the doorway sound's level).
- **Stutter (Chris heard fluttering after pathing went in):** two causes. `deploy.ps1` built **Debug** by default, and the Debug engine renders the reflections alone at 60 % p50 / 100 % p99 of the block on 7.1.4 (Release: 10 % / 14 %); pathing's share on top made it underrun. The deploy now builds Release (`-Configuration Debug` for asserts). And the path was a hard gate at occlusion 0.9 on a noisy estimate, so a partly seen sound flapped between direct and direct + path (Steam Audio answers a clear centre ray with the direct path itself, which doubles a seen sound); the path is now weighted by how blocked the sound is, full at 0 and nothing at 0.9.
- **Volume dropping in doorways (Chris):** `VoxelView::escape` moved a point out of any cell holding a partial block, so the listener walking through an open door (the leaf is a partial block beside the head) was thrown a metre to the far face of the door cell, towards wherever they looked, and the wall came between them and the sounds on the side they came from. The listener now escapes only what encloses the head (`Escaping::Enclosures`: a solid cell, or a partial block's box the point is inside, leaving the box rather than the cell). Sounds keep the wide rule (`Escaping::Blocks`): a door's own sound sits at the door block's centre, which is not inside the leaf, and narrowing the rule for sounds too put the leaf between the door and the player opening it (Chris heard doors from behind their leaf). `test_transmission.cpp` (the listener beside the leaf stays, inside it comes out of the leaf; the door's sound leaves the cell past the leaf) and `test_pathing.cpp` (walking in through a door: nothing drops, the listener stays put) keep it.
- **Config migration:** the file is rewritten with every default, so Chris's held `ReflectionGain: 1.0` from before the default became 0.1. `SpatialAudioConfig.ConfigVersion` (stamped on load) and `Migrate()` replace a changed default where the file still holds the old one; a chosen value is kept. Add a step there when a default changes again.

### Native (ABI v11)

- `world/path_baker.*` (`PathBaker`): probes (`UNIFORMFLOOR`, 2.5 m, 1.6 m high), the bake (radius 1 m, threshold 0.1, visibility range a third of the range, path range the range, one visibility sample), `current()` the latest batch, `stats()`.
- `world/path_sim.*` (`PathSimulator`): its own simulator (PATHING, `maxOrder` 1) at `pathing_rate_hz` (10); swaps the baker's batch in with a commit and releases the old one after; the `pathing_sources` (16) loudest sounds that want a path (occlusion below 0.9, or no direct simulation); `enableValidation` and `findAlternatePaths`; order-1 coefficients and EQ out through `world/path_channel.hpp`; the legs Steam Audio considered, from its visualisation callback, for the overlay.
- **Mixer:** `PathState` per effect set, one `IPLPathEffect` each (order 1, not spatialised) into an order-1 world-space Ambisonic path bus; on headphones it joins the world bus's binaural decode, on speakers its own `SpeakerDecoder`. The effect is primed muted until the first result; a sound that stops wanting a path fades out over the direct smoothing.
- **API:** `vsa_engine_get_pathing_stats`, `vsa_engine_get_path_segments`; config `pathing_range` (32..256), `pathing_height` (16..128), `pathing_probe_spacing`, `pathing_vis_samples`, `pathing_rate_hz`, `pathing_sources`; flag `VSA_ENGINE_FLAG_NO_PATHING`. Test fixtures default to `NO_REFLECTIONS | NO_PATHING`; the pathing tests turn it on.
- **Tests:** `test_pathing.cpp` (the box bakes and re-bakes; goat and doorway: energy leans 0.60 ahead and 0.00 sideways with paths against 0.30 left without, 79 dB louder; a sealed room has no path, a sound in the open wants none); `core/test_render_budget.cpp` (the pathing render path never allocates through moves, stops, decoder and output changes and a re-bake; 32 blocked voices with 16 paths render at p99 7 % headphones / 5 % 7.1.4 in Release, simulation 0.1 ms); `core/test_pathing_bake.cpp` keeps the bake budget.

### Managed

- Config: `Pathing` (on), `PathingRangeBlocks`, `PathingHeightBlocks`, `PathingProbeSpacing`, `PathingVisibilitySamples`, `PathingRateHz`, `PathingSources` (0 = the engine's defaults).
- `.spatialaudio scene paths` (also in the overlay cycle) draws the legs Steam Audio considered in the last run: occluded legs in red. The HUD has a pathing line (batch, probes, bake time, wanted / simulated / found, tick time).
- Reverb: `ReflectionGain` now defaults to 0.1, with `ReflectionEarlyGain` and `ReflectionTailGain` (`.spatialaudio reverb early N` / `tail N`) to weigh the convolved early part against the diffuse tail.
- `PathingTests.cs`: config clamping; a room with a doorway bakes and a blocked sound finds its way out through the real engine.

### To check in game (Chris)

- **Goat and doorway:** stand outside a closed room with a sound inside (an anvil, an animal), off to one side of the doorway. The sound should come from the doorway, not through the wall from the sound's true direction; walk round the room and it should follow the doorway. Then close the doorway: it should go back to a muffled sound through the wall.
- **Caves:** sounds round a bend should come from the bend.
- **Overlay:** `.spatialaudio scene paths` while a sound is blocked; the HUD's pathing line should show a bake within a second of arriving somewhere new, and `found` counting the blocked sounds.
- **Cost:** the HUD's bake time when walking (the box re-bakes every ~32 blocks) and `.spatialaudio stats` render time with many blocked sounds.
- **Not yet:** a memory cap and a cancellable bake (the default box is small; deferred until they matter).

## Phase 6 (reflections and reverb): done, merged

Phase 5 is merged into `main` (not pushed). The design, and why it differs from PLAN §5.4, is ADR 0009.

### Native (ABI v8)

- **`world/reflection_sim.*` (`ReflectionSimulator`):**
  - Steam Audio real-time reflections (HYBRID simulation) for a fixed pool of sources that live as long as the simulator.
  - Slot 0 sits at the listener: the reverb every other world sound shares. Slots 1..N follow the voices the mixer gives them.
  - Runs on its own thread at up to `rate_hz`, resting at least as long as each run took (so it is busy half the time at most); runs synchronously offline.
  - Each run simulates the listener's slot plus at most half the voice slots (new voices first, then round-robin).
  - Rebuilt when the output's sample rate changes (its impulse responses are partitioned for the render block).
- **`world/reflection_channel.hpp`:** lock-free per-slot inputs (position, voice, generation) and outputs:
  - the hybrid RT60 / EQ / delay, published under the generation;
  - `observed`, the input generation the last run started with.
- **`audio/reflections.*` (`ReflectionRenderer`):** per slot:
  - a Steam Audio CONVOLUTION effect over the first `transition` seconds;
  - our `dsp::LateReverb` tail: 16-line FDN, per-band decay, 8 independent outputs as plane waves from a cube's corners.
  - Everything goes into a world-space Ambisonic bus. A released slot drains and is reused only once `observed == 0` and one pass has consumed any response in flight.
- **`audio/speaker_decoder.*`:** AllRAD for every speaker layout (VBAP onto the real speakers, max-rE, normalised to a panned voice's power), following the head per block.
- **Mixer:**
  - ranks voices for slots by level without walls;
  - short sounds (< 0.75 s, one-shot) always use the listener's reverb;
  - sends are taken after voice gain, fades and the bus gain;
  - a voice cross-fades from the shared reverb to its own slot over 8 blocks.
  - **Headphones:** the reflections join the world bus's binaural decode, which now runs only at the highest order present (order 2 when only reflections are there).
  - **Speakers:** the speaker decoder.
  - `vsa_engine_set_reflection_gain`.
- **`WorldScene`'s lock is now a writer-preferring shared lock (`SceneLock`):** the direct and reflection simulations trace at once; edits wait at most one run.
- **Config:**
  - `reflection_sources`, `_rays`, `_bounces`, `_duration`, `_order`, `_rate_hz`, `_threads`, `_transition`;
  - defaults are the Medium preset (8 / 2048 / 16 / 1.0 s / order 2 / 10 Hz / 0.1 s);
  - flag `VSA_ENGINE_FLAG_NO_REFLECTIONS`.
- **Debug:**
  - `vsa_engine_get_reflection_stats`: RT60 at the listener per band, output level, slots live/waiting/draining, timings;
  - `vsa_engine_get_reflection_sources`;
  - `vsa_scene_trace_rays`: deterministic specular or scattered paths off the voxels, losing each surface's absorption.
- **Tests:**
  - **Golden RT60, `test_reflections.cpp`:** closed stone rooms at 20% absorption give 0.40 / 1.08 / 2.29 s against Eyring's 0.43 / 1.08 / 2.43. Also: the open field (13 dB less), the rendered decay against the simulated one, slots and hand-over, gain, validation, and ray paths.
  - **Tail calibration, `core/test_reflection_calibration.cpp`:** against Steam Audio's full convolution response in five rooms.
  - **Late reverb, `core/test_late_reverb.cpp`:** RT per band, decorrelation, level, silence, no allocation.
  - **Speaker decoder, `core/test_speaker_decoder.cpp`:** even power per layout, 7.1.4 heights, direction, head tracking.
  - **Voxel ray query, `core/test_transmission.cpp`:** first hit.
  - **Budgets, `core/test_render_budget.cpp`:**
    - the reflections' render path allocates nothing (slot churn, mode switch, 7.1.4 reopen);
    - Medium within PLAN's budget: render p99 about 17% (headphones) and 13% (7.1.4) of the block, a run about 27 ms.
  - The engine test fixture turns reflections off by default: they would add reverb to every level the older tests measure. SceneLab turns them off too (no geometry; offline they would count in its block load).

### Managed

- **Config (`spatialaudio.json`):**
  - `Reflections` (on);
  - `ReflectionQuality` (Low / Medium / High / Ultra, `Config/ReflectionPresets.cs`);
  - per-value overrides (`ReflectionSources`, `ReflectionRays`, `ReflectionBounces`, `ReflectionDurationSeconds`, `ReflectionOrder`, `ReflectionRateHz`, `ReflectionThreads`, `ReflectionTransitionSeconds`; 0 = the preset's);
  - `ReflectionGain` (1).
- **`.spatialaudio reverb [status|gain N|rays]`.**
- **Overlay "reflections"** (in the Ctrl+F7 cycle, or `.spatialaudio scene rays`):
  - 48 sound paths from your head bouncing off the scene, bright cyan fading to dark blue as surfaces absorb them;
  - a magenta line and diamond to each voice with reflections of its own.
- **The HUD's reflection lines:**
  - RT60 where you are per band, with a name for the space ("a small room", "a hall", "a cave or cathedral");
  - the reflections' level;
  - slots live/waiting/fading;
  - the simulation's timings and settings;
  - with the overlay on, each voice with its own reflections and its RT60.
- **Vanilla's `SetReverb`** is recorded and ignored. There is no fallback reverb when reflections are off.

### After Chris's first test (ADR 0010)

- **What Chris found:**
  - reverb far too strong even at gain 0.1;
  - phasing from early reflections;
  - a wooden house sounding like a cave;
  - far animals reverberating in his room, as if through walls.
- **Causes:**
  - the listener's shared reverb was fed by distance alone, ignoring walls;
  - every send was scaled by vanilla's reference distance (3 m and more, so +10 dB);
  - the listener's reverb had early reflections;
  - Steam Audio's bare-panel wood preset.
- **Fixes:**
  - the listener's reverb gets each sound at its direct path's level (walls included) and has only a tail;
  - sends are scaled by `clamp(distance, 1, min_distance)`;
  - short sounds share "spots" simulated where they happen (4 m, kept 20 s after the last sound), so an animal's calls ring in its own room with walls respected by Steam Audio;
  - building materials absorb more and rough ones scatter more.
- **Tests:**
  - a sound outside a closed room adds nothing to the listener's reverb;
  - reverb keeps the direct sound's ratio for any reference distance;
  - spots are made, shared and let go.
- **Open:** the direct sound has no propagation delay, so far sounds' reflections come late after it (58 ms at 20 m). A physical propagation delay would fix that.

### After Chris's second test

- **Reported:**
  - still strong (gain 0.2);
  - poor, jumpy quality;
  - an anvil behind a block heard only by reflections, at very inconsistent levels, sometimes none;
  - walls too absolute for the direct sound.
- **Fixes:**
  - **Tail smoothing:** the tail's level and decay time are smoothed in the log domain over 0.7 s and 1 s (before: ~20 ms). Each run's estimate is noisy.
  - **Onsets:** a sound's onset goes straight to its ready slot or spot. The old 43 ms crossfade from the listener's reverb (which gets nothing from behind a wall) lost a strike's attack.
  - **Holding positions:** the reflection simulation holds the listener's and each source's position until they move 0.5 m. Steam Audio averages its runs only while nothing moves at all, so the noise now settles.
  - **Rays doubled in every preset:** Medium is 4096. They are cheap next to impulse-response rebuilding.
  - **Separate controls:** `vsa_engine_set_reflection_mix(early, tail)`, `.spatialaudio reverb early N` / `tail N`, and `ReflectionEarlyGain` / `ReflectionTailGain` in the config.
  - **Direct sound through materials:** half the dB (stone 27.5 dB mid-band per block, was 55).
- **Tests:**
  - a strike behind a wall has the same reflection level on every repeat (-19 dB on strikes 2–6; the first has none, its spot not existing yet);
  - a 6 dB level change reaches the tail in steps of at most 0.5 dB per 50 ms;
  - early and tail can each be turned off.
- **Materials pulled apart** (wood and stone sounded alike after the first retune): stone 0.07 / 0.10 / 0.13 absorbed, brick 0.08 / 0.11 / 0.14, wood 0.20 / 0.30 / 0.33. A 7×4×7 room decays in about 0.4 s built of wood and 1.4 s of stone; a large cave in about 5 s.
- **Worth knowing:** in a small room about 97% of the reflected energy is early reflections (the first 0.1 s). `.spatialaudio reverb early 0` shows how much is them.

### Back to the plan (ADR 0011)

- **Reported:** reverb volume jumping from hit to hit on an anvil in a small room.
- **Measured:** Steam Audio's output is identical from run to run. The cause was our routing: own slots, spots, or a tail-only listener reverb, with about 15 dB between them, chosen by slot bookkeeping.
- **Now:**
  - one listener reverb (early reflections and tail) for every sound;
  - each sound feeds it by the louder of its direct path and its shortest path round obstacles through the air (`world/air_paths.*`, recomputed when the listener changes block);
  - spots removed;
  - voices' own reflections optional (`VoiceReflections`, ABI v9 `reflection_sources` 0 = none, the default).
- **Tests:**
  - six strikes alike, first included (in the open and behind a pillar);
  - air paths: straight in the open, round a pillar, through a doorway, dearer through a door, none out of a sealed room or between diagonal blocks.

### Steam Audio's own way (ADR 0012)

- **Reported (ADR 0011's single reverb):** no directionality, and leaks through walls again.
- **Now:** every world sound is simulated from where it is (`world/reflection_sim.*` places; `Mixer::assign_place`), no fallback path at all. Sounds within 3 m share a place; places are kept while used (converging), taken over by the longest-idle or a much louder sound, let go after 30 s idle. A new place's first result runs at once (the simulation thread polls every 4 ms); its effects run muted until then so the onset is in their history.
- **Removed:** the listener reverb for world sounds, spots, `AirField` and `air_path` (ABI v10), `VoiceReflections`. `ReflectionSources` = places (Medium 10, Low 8, High 20, Ultra 32).
- **Fixed on the way:** the tail's smoothing primed on a new place's empty parameters and crept up from silence (a click at a new place had no tail).
- **Measured:** six strikes alike from the first (-16 dB open, -19 dB behind a pillar); rendered decay matches simulated; Medium's 10 live places with 32 voices: render p99 18-21 % (budget 25 %; 12 places 22-28 %), a run ~45 ms. Each live place costs ~55 us per block: the next optimisation.

### Directionality, measured (after Chris's "no panning" report)

- A hole in the left or right wall of a room built round the listener, anvil outside, sounded the same.
- **Measured (7.1.4, `test_reflections.cpp` / `test_speaker_decoder.cpp`):** a plane wave decodes at 16 dB left over right at order 2 (18.5 at order 3); a click 3 m to the side has its early reflections lean 5.5 dB to that side in the first 40 ms; over 30-150 ms the lean is 1.5 dB (the room's field is diffuse by then, and the tail is diffuse by design). So the rendering is directional; the cue is short.
- **Why the hole test hears nothing:** reflections carry only what rays through the hole find; the sound "coming through an opening from the opening's direction" is diffraction, which is Steam Audio's *pathing* (PLAN 5.5, Phase 7), not its reflections. Meanwhile the direct sound transmitted through the wall arrives from the anvil's true direction in both cases.
- **Knobs:** `ReflectionTransitionSeconds` (0.1) lengthens the directional, convolved part at CPU cost; `ReflectionTailGain` lowers the diffuse part.

### Level

- `ReflectionGain` defaults to 0.1 (-20 dB): what Chris finds comfortable on the 7.1.4 system with the current calibration (1 = the level Steam Audio simulates). Worth revisiting if the calibration or the materials change.

### To check in game (Chris)

- **Reverb that follows the space:** walk from outdoors into a small stone room, a big hall, a cave. The HUD's RT60 and space name should follow; outdoors should be nearly dry.
- **The occlusion feel from Phase 5:** is it better now that sound also arrives by reflections? If reverb is too much or too little overall, try `.spatialaudio reverb gain 0.5` or `2` and report which sounds right.
- **Reflections overlay:** paths should stay inside rooms and escape to the sky outdoors. Magenta lines should go to the loud, lasting sounds (a fire, a trader's music, rain?).
- **Speakers:** reverb should surround you on the 7.1.4 system (including the heights), not sit in the centre.
- **Stats:** note `.spatialaudio stats` render time and the HUD's simulation time in a busy place.

## Phase 5 (direct simulation): in progress on `phase5-direct-simulation`

Phase 4 is merged into `main` (not pushed).

### Native (ABI v6)

- `world/transmission.*` walks the voxels from source to listener (Amanatides–Woo). Each material entered costs its crossing loss; each metre inside costs its bulk loss; partial blocks' boxes are intersected exactly (ADR 0008 amends ADR 0004). `VoxelView` is an immutable snapshot of shared chunk grids plus material losses and the origin, cached by `WorldScene::voxel_view()` and rebuilt only after a change.
- **Escaping:** sounds at the centre of a solid block (block sounds) are moved out of it towards the listener, by up to two cells, before simulation.
- `world/direct_sim.*` is `DirectSimulator`. It has one Steam Audio source per effect set in use, volumetric occlusion (16 rays by default), and the voxel transmission. It runs on its own thread at 30 Hz while a device plays, and synchronously from offline rendering on the output's clock, so tests are deterministic. It commits the simulator only when sources or the scene changed. Its steady state allocates nothing (tested).
- `world/direct_channel.hpp` holds the lock-free per-set slots:
  - **inputs:** position, radius, voice, and a generation the mixer bumps when a set changes voices;
  - **outputs:** occlusion and transmission per band, published under that generation.
- **Mixer:**
  - It publishes world voices' positions and smooths results over about 60 ms.
  - `gain = occlusion + (1 − occlusion) · T` goes to Steam Audio's direct effect (frequency-dependent transmission).
  - A voice's **onset** waits up to 80 ms for its first result when the scene has chunks, so nothing blips at full level behind a wall.
  - Occlusion affects **ranking** (binaural budget, stealing) but not **virtualisation**, so occluded voices stay simulated and are heard the moment a door opens. The first attempt virtualised them, which oscillated.
  - Head-locked voices aren't simulated.
- Config `occlusion_samples` and `direct_rate_hz`; flag `VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION`. Debug: `vsa_engine_get_sources`, `vsa_engine_get_simulation_stats`.
- Tests (`test_transmission.cpp`, `test_direct.cpp`):
  - The thickness matrix (glass < wood < 1 < 3 < 6 stone, every band) in voxels.
  - End to end through Steam Audio. Glass measures 17/26/34 dB, one stone 40/54/59 dB and three stone 65/83/85 dB. Steam Audio's EQ limits the deepest losses.
  - No onset blip; a smooth corner; escaping; head-locked voices unaffected; source readout.

- **Steam Audio 4.8.1 + Embree bug** (root cause from its source, `embree_scene.cpp` / `embree_static_mesh.cpp`): a released mesh hands its geometry id back (`releaseGeometryID`) without detaching its Embree geometry. The next mesh gets that id, `rtcAttachGeometryByID` fails, and the new geometry is silently missing. In game, occlusion vanished once doors had been opened or chunks had streamed.
  - **Workaround (`WorldScene`):** edits change the top-level scene in place, but nothing removed is released while that scene lives. Removed instances and the chunk builds they reference wait in the scene's graveyard.
  - **Compaction:** when the graveyard reaches max(64, live/4), a fresh top-level scene is built with no lock held; attached simulators (`WorldScene::attach`) switch to it, and the old one is released whole.
  - **Cost:** an edit takes about 1 ms median in Release on a 405-chunk scene. Compaction (about 24 ms) happens roughly once per 100 edits.
  - **Tests:** `core/test_embree_scene_edits.cpp` reports the in-place behaviour and checks both the in-place-without-release pattern and full rebuilds; `test_direct.cpp` checks 10 rebuilds with chunks coming and going, on both ray tracers.
- **Sources inside their own block, full or partial** (an anvil), leave it towards the listener and end 0.25 m past its face. Their occlusion sphere is then capped at 0.25 m, so the block doesn't hide its own sound.
- **From Chris's first in-game test:**
  - The listener's backward offset is now `vsa_listener.render_offset` (ABI v7), for rendering only. The simulation listens from the eyes, and leaves solid blocks (third-person camera).
  - Sources keep their radius off neighbouring solid faces (floor sounds).
  - A run through full cells shorter than 25 cm pays its crossing loss in proportion (grazing corners).
  - Vanilla's range check in `PlaySoundAtInternal` is scaled by `SoundRangeMultiplier` (3×; the anvil stopped dead at 12–16 m).

### Managed

- `Occlusion`, `OcclusionSamples` and `OcclusionRateHz` in the config. The shipped materials were retuned to crossing and bulk losses (documented in the JSON).
- **Overlay "sources":** in the Ctrl+F7 cycle and `.spatialaudio scene sources`. It draws a line to every simulated sound, through walls, coloured green (clear) → yellow (−20 dB) → red (−40 dB), with a white stub where a sound was moved out of its block. The HUD adds the simulation's timings and the six nearest sounds: asset name, distance, visible fraction, metres and materials in the way, and the resulting dB per band.

### To check in game (Chris)

- Walk around a house with something making noise inside (a chicken, a fire, a door): muffled through walls, clear through an open door, gradual around corners, more muffled through stone than wood or glass.
- Watch the sources overlay and HUD for anything that looks wrong (a clear line through rock, a red line in the open), and note the simulation tick time with many sounds around.

## Phase 4 (world geometry): in progress on `phase4-world-geometry`

Phases 2 and 3 are merged into `main` (not pushed; Linux and macOS CI deferred to the end, at Chris's request). The design change from the plan is recorded in ADR 0007.

### Native (ABI v5)

- `vsa_scene_*`: a material table (`vsa_acoustic_material`: kind, Steam Audio surface bands, dB/m attenuation for Phase 5, name). Chunks arrive as `vsa_chunk_desc`: 32³ `uint16` material ids plus partial blocks' boxes, at LOD 0 or 1. Also origin, stats, mesh read-back with a version, a chunk list, OBJ export, and `wait_idle` for tests.
- `world/mesher.*` is the boundary-only greedy mesher. A surface is emitted where a denser kind meets a more open one (air < liquid < porous < solid). Partial blocks mesh as their boxes; LOD 1 uses 2³ majority super-voxels; faces toward unknown chunks are left out. It takes about 1 ms per terrain chunk in Release (7.6 ms for pathological noise).
- `world/world_scene.*` meshes on its own thread and instances each chunk (a sub-scene with one static mesh) in the top-level scene. It re-meshes neighbours when a chunk arrives or leaves; after an edit, only neighbours whose shared layer changed. Instances sit at chunk minus origin. Remove + Commit happens before Release. The simulator (Phase 5) must hold `scene_lock()` while it runs.
- Verified with real Steam Audio occlusion rays across a chunk's wall, including after moving the origin (`core/test_world_scene.cpp`).

### Managed

- `World/MaterialTable`: `assets/spatialaudio/config/acousticmaterials.json` holds materials, a block-material map and code wildcards. A `ModConfig/spatialaudio-materials.json` override is re-read by `.spatialaudio scene reload`.
- `World/BlockClassifier`:
  - Plants, fire and blocks without collision boxes are air.
  - Leaves and liquids fill their cell (leaves have no collision boxes).
  - Solids are full cubes or their boxes, clamped to the cell. Boxes on blocks with a block entity (doors) are read per snapshot, on the main thread.
- `World/ChunkReader`: `Unpack_ReadOnly`, then a bulk read lock, `GetBlockIdUnsafe`, and the fluid layer where the solid block isn't in the scene.
- `World/ChunkStreamer` keeps full detail within 2 chunks and a coarse ring out to 4, ±2 chunks vertically, nearest first. Dirty chunks are debounced for 250 ms and fingerprinted, so unchanged re-reads aren't re-sent. There is no client unload event, so sent chunks are polled every second.
- `World/WorldAcoustics` starts at `LevelFinalize` and spends 2 ms per 50 ms tick. It hooks `ChunkDirty` (thread-safe queue) and `BlockChanged`; around block entities it also marks nearby chunks, for multi-block gates.
- The origin is chunk-aligned and moves when the player strays 1024 blocks. `AudioSession.SetOrigin` re-sends the listener and every world sound relative to it.
- Config: `BuildWorldScene`, `SceneFullRadiusChunks`, `SceneLodRadiusChunks`, `SceneVerticalRadiusChunks`, `SceneBudgetMs`.

### Visual debugging

- **Ctrl+F7** (rebindable) cycles the overlay: off → wireframe → wireframe + chunk bounds → translucent faces + wireframe.
- The wireframe is the mesh read back from the engine (exactly what Steam Audio has), coloured by material, and uses the game's own wireframe shader. Chunk bounds are green for full detail, blue for coarse and grey for empty.
- A HUD panel shows scene stats (chunks, triangles, memory, meshing and commit times), streaming state, the origin, and the acoustic material of the block under the crosshair.
- `.spatialaudio scene` takes `status | wire | faces | bounds | off | radius N | legend | export | reload`. `export` writes an OBJ + MTL to the Logs folder.
- **Ray probe** (whenever the overlay is on): a ray from the camera along the view, tested against the scene's meshes as submitted (`vsa_scene_raycast`, native, Möller–Trumbore per chunk). The hit triangle and the block that produced it are highlighted in yellow. The HUD gives distance, material, whether it came from a whole cell's face or a partial block's box, the chunk and triangle, and the game block at that cell (code, class, block material, block entity, fluid, and a multi-block filler's control block).
- The probe's first catch: a door's upper half is a `BlockMultiblock` filler, whose static collision box is a full cube, so it had become a solid wooden block. Fillers (`IMultiblockOffset`) are now dynamic: their real boxes come from the door, and their material from its control block.

### To check in game (Chris)

- Press Ctrl+F7 in a varied area (cave, house, water, trees). The wireframe should hug the terrain and buildings; materials should look right (look at blocks with the HUD open); doors should change when opened and closed.
- Watch the HUD's tick time and meshing times while flying around. Note triangle counts and memory for the budget (an exit criterion).
- Edit latency (edit to scene within 250 ms, an exit criterion): place and break blocks with the wireframe on.

### Still to do for Phase 4

- Measure triangle and memory budgets on real worlds, and set presets from them.
- Consider moving chunk reading off the main thread if the 2 ms budget is too slow to fill the scene.

## Phase 2 (engine takeover): in progress on `phase2-takeover`

Built and tested offline; **not yet run in the game**. Phase 1 is merged into `main`.

### Native (ABI v3)

- Positional voices (`vsa_voice_desc.spatial` world/listener, `vsa_voice_set_position`) render mono through Steam Audio's direct effect (1/r beyond a per-voice minimum distance, 3-band air absorption), then binaural (headphones) or panning (speakers). `vsa_listener_set`, `vsa_engine_set_render_mode`.
- Effect pool (`max_real_voices`, 256) created off the audio thread; when full, the quietest positional voice loses its set. Binaural budget (`max_binaural_voices`, 64): the loudest voices get HRTF, the rest go to the world Ambisonic bus (Phase 3, below). 256 binaural voices cost ~48 % of the block (p50), with the budget ~37 %, panned ~25 %.
- Virtualisation: voices estimated below -70 dB advance without rendering (real again above -64 dB). Streams are never virtual.
- `vsa_voice_set_lowpass`: the game's EFX low-pass (underwater) as OpenAL Soft implements it, a 5 kHz high shelf.
- **Steam Audio's HRTF only exists at 24, 44.1 and 48 kHz**, so the engine renders at 44.1 or 48 kHz; devices at other rates get a converted stream (miniaudio), and the offline output accepts only those two rates.

### Managed takeover (`src/VintageStorySpatialAudio/Takeover/`)

- `AudioTakeover` (in `StartPre`): verify, open our device (matching the game's `audioDevice` setting), Harmony-patch the platform seam, carry the menu music over if it is still playing, dispose vanilla sources and close OpenAL. Hand-back in `Dispose`: release voices, unpatch, `Unload()` the samples we emptied so vanilla decodes them again, reopen OpenAL via the original `StartAudio()`.
- `SpatialAudioSound`: `ILoadedSound` with vanilla's contract (restart on Start, fade clamping and main-thread callbacks, SetVolume not cancelling fades, deferred start while loading). **`AudioMetaData.Loaded` must be set to 3 when a sound is created**: `PlaySoundAt` only starts sounds whose data reached 3.
- `CreateAudioData` decodes natively and returns metadata with an empty `Pcm`; long Ogg files stream.
- Category sliders become bus gains (polled every 250 ms), `masterSoundLevel` the master gain, `useHRTFaudio` the render mode; `CategoryTrimDb` in the config trims each category for rebalancing by ear.
- The 250-sound cap is removed with a transpiler on `PlaySoundAtInternal` (reported by `.spatialaudio stats`).
- The mod now compiles against VintagestoryLib and the game's 0Harmony (not shipped). Everything touched is in `AudioPatchTargets` (35 entries, verified before patching).

### Verified in the game (19 Sep 2026)

Chris played a session on the AV receiver (48 kHz, 6 channels, speakers mode): takeover ACTIVE, 35/35, menu music carried over at 43.5 s, sounds all working, clean hand-back to vanilla at the menu. HRTF not tried (no headphones). The takeover now also refuses to run when another mod has Harmony patches on our targets or VintageStorySurroundSound/AcousticLab is enabled.

### Surround (first Phase 3 item, done early)

Speakers mode pans positional voices to the whole output layout (quad, 5.1, 7.1) with Steam Audio's panning effect. Buses, master and the limiter are N-channel (limiter linked). Channels are routed by speaker using miniaudio's channel map for the device, so a 5.1 device with side instead of rear surrounds still gets the rear channels; the log's "output:" line shows the device order. Unpositioned sounds and binaural voices stay on the front pair; the LFE is unused. Tests: `test_channel_layout.cpp`, and the 5.1/7.1/quad direction cases in `test_spatial.cpp`.

## Phase 3 (output formats): done on `phase2-takeover`

**Verified in the game (19 Sep 2026)**: with speakers, the output ran through Windows Spatial Audio and `.spatialaudio speakertest` was "perfect" on Chris's Atmos receiver, heights included. Headphone rendering (the ambisonic tier, SOFA) is covered by tests only; Chris has no headphones.

### World Ambisonic bus (headphones, beyond the binaural budget)

- Voices past the binaural budget are encoded into one **world-space order-3 Ambisonic bus** (16 channels) and decoded binaurally once per block with the listener's orientation (`SpatialRenderer::encode/decode`, tier `SpatialTier::Ambisonic`). Head rotation doesn't touch the encoding; each voice's coefficients ramp across the block when it moves.
- Encoding is ours (`dsp/spherical_harmonics.hpp`), not Steam Audio's encode effect: that effect cost ~5 µs per voice per block, and **its first block after a reset scales channel c by c/frameSize** (a transition bug; the steady state is fine). Our coefficients match its steady state exactly (`core/test_spherical_harmonics.cpp`): orthonormal real SH, ACN, ambisonic axes x = -z, y = -x, z = y.
- Order 3, not 2: order 2 lost 3-5 dB at 1 kHz for frontal sources and had deep comb notches. Even at order 3 the binaural decode is **4.7 dB below per-voice HRTF, averaged over the sphere** (0 dB to the sides, 5-7 dB elsewhere, worst at 3 kHz); `kAmbisonicMakeup` restores the diffuse-field level so a voice doesn't jump when it changes tier (a test holds it within 1 dB).
- A source straight behind leans 4.5 dB left at 6-8 kHz through the bus. It's the same whichever way the listener faces, so it's in Steam Audio's order-3 HRTF, not our coordinates.
- At the head (`spatial_blend` < 1) the directional channels fade out, leaving the omnidirectional W, so the source sounds centred.
- Bus gains are rendered per frame at the block start, because ambisonic voices from every category share the one bus; its decode is added to the master front pair before master gain.
- Tests (`test_spatial.cpp`, "headphone tiers"): left/right and turning, above/below with the head tilted, looking straight up, front vs back brightness, bus gain, centring and the diffuse level match, for both the binaural and ambisonic tiers.

### 7.1.4 layout (12 channels)

- The engine's 7.1.4 order is FL FR FC LFE BL BR SL SR TFL TFR TBL TBR (7.1 plus the heights, which is Windows' channel-mask order). Buses, master and the limiter go up to 12 channels; miniaudio devices that report 12 channels with height positions get it natively.
- Positional voices are panned with our own 3D VBAP (`audio/vbap.*`), not Steam Audio's panning effect. It uses ITU/Dolby placements (FL/FR ±30°, SL/SR ±90°, BL/BR ±150°, heights at ±45°/±135° azimuth and 45° elevation). Virtual speakers keep it symmetric: the zenith spreads over the four heights, the nadir over the ear-level ring, and the centre of the back quad (BL BR TBL TBR are coplanar, since there's no back-centre speaker) over those four. Without that virtual speaker, the quad's two triangulations swapped and gains jumped by 0.65 behind the listener. Gains ramp across each block.
- Tests: `core/test_vbap.cpp` (speaker directions, zenith and nadir spread, back symmetry, power, continuity around circles at seven elevations) and the 7.1.4 cases in `test_spatial.cpp`: front, overhead (100 % in the heights), above-front, below, behind, left and above-behind-right, turning, looking up and down, and a source gliding overhead without zipper noise.

### Windows Spatial Audio output (ABI v4)

- `VSA_OUTPUT_SPATIAL` (`backend/spatial_output.*`): `ISpatialAudioClient` on the device (miniaudio's id holds the WASAPI endpoint id), a static 7.1.4 bed (all 12 objects, mask 0x1ffe, `AudioCategory_GameEffects`), the offered object format (float32 mono, 44.1/48 kHz only), and `GetMaxFrameCount` frames per update. It runs on its own thread (MTA, MMCSS "Pro Audio"): wait on the event, Begin, render, copy each engine channel into its object's buffer, End.
- The activation `PROPVARIANT` borrows our parameters and is never cleared (OpenAL Soft's heap-corruption bug). A 500 ms wait timeout or any failed update marks the output lost; the worker recreates the stream rather than calling `Reset` (0x88890100 on the receiver). A default-device change while following the default reopens the stream on the new device.
- If spatial audio is unavailable (not Windows, no spatial sound format enabled, unsupported format), the engine opens the plain device instead, now and on every reopen; `stats.output_kind` says which is running. There is no periodic retry, so enabling Atmos mid-session takes effect at the next reopen.
- **Verified on Chris's machine by `test_output.cpp`**: 'AV Receiver (NVIDIA High Definition Audio)', 48000 Hz, 12 channels, 480-frame updates (the plain device path gives 8 channels).
- Managed: `SpatialAudio` config option (default on). The takeover opens the spatial output in speakers mode and the plain device with the game's HRTF option on, since our binaural render must not be spatialised again by Sonic or Atmos for headphones. It reopens when that option changes. `.spatialaudio stats` shows "via Windows Spatial Audio (7.1.4)".
- `.spatialaudio speakertest` plays a noise burst from each 7.1.4 position in turn, named in chat, then straight overhead. Use it with the receiver's display to confirm the heights.
- SceneLab writes `WAVE_FORMAT_EXTENSIBLE` with the speaker mask for more than 2 channels; `scenarios/surround-714.json` is a 7.1.4 listening scene.

### SOFA HRTF

- `vsa_engine_config.hrtf_sofa_path` (the config struct is now 72 bytes, with a `reserved` field that must be 0 at offset 60) and the `HrtfSofaFile` config option (absolute, or relative to ModConfig). If it can't be loaded, the engine logs a warning and uses the default HRTF. Tested with a missing file, a junk file, and a real one: MIT KEMAR (`third_party/sofa`, pinned in `deps.json`, test-only). It loads at 48 kHz (Steam Audio resamples the 44.1 kHz data) and gives ±10 dB between the ears for sources at the sides.

### Left open from Phase 3

- Dynamic spatial objects (the loudest sources as Atmos objects rather than panned into the bed): only if the bed turns out not to be enough.

### Still to do for Phase 2

- In-game verification: a full session with no missing, stuck or misbehaving sounds; category rebalance by ear; join/leave cycles (menu audio must work after leaving).
- Positional stereo assets are downmixed to mono (the plan's two-emitter wide source is deferred).
- Detecting other mods' Harmony patches on our targets (VintageStorySurroundSound) and refusing the takeover.

## Phase 1 as built

### ABI (v2, `native/include/vsaudio.h`)

- Engine config gained `sample_rate`, `block_frames`, `max_voices`, `resampler_quality`, `stream_threshold_ms` (0 = default for each).
- Assets: `vsa_asset_create/get_info/release`. Reference counted and independent of the engine's lifetime (`vsa_asset_release` takes no engine).
- Voices: `vsa_voice_create/release/start/pause/stop/set_gain/set_pitch/set_looping/seek/fade/get_status`; handles are `generation << 32 | slot`, never 0.
- Buses and master: `vsa_bus_set_gain`, `vsa_engine_set_master_gain`.
- Output: `vsa_device_enumerate`, `vsa_output_open` (NONE or DEVICE), `vsa_engine_render_offline`, `vsa_engine_get_stats`.
- Events: `vsa_engine_poll_events` (fade done/cancelled, voice ended, stream underrun, device rerouted/lost/restored).
- Layouts are pinned three times: `static_assert`s in `api.cpp`, `NativeLayoutTests.cs`, and struct_size checks at runtime.

### Native layout

| Path | What |
|---|---|
| `src/engine.*` | Owns everything; API-side validation, voice slot allocation, worker thread, output switching and device recovery |
| `src/audio/mixer.*` | The render core (render thread only): commands → voices → buses → master → limiter → output layout |
| `src/audio/voice.hpp` | Command POD, voice slot (atomics shared with API threads + render-only state) |
| `src/audio/asset.*`, `src/audio/stream.*` | Assets (int16 decoded or encoded Ogg), per-voice decode-ahead streams |
| `src/decode/decoders.*` | WAV (PCM 8/16/24/32, float 32/64, extensible) and Ogg Vorbis over memory |
| `src/dsp/` | `resampler` (bandlimited interpolation), `limiter` (true-peak look-ahead), `gain_ramp` (linear / dB-linear), `simd.hpp` (SSE2/NEON) |
| `src/backend/device.*` | miniaudio playback device, enumeration, hot-plug flags |
| `src/core/` | `spsc_ring.hpp`, `rt_log` (render-thread log records), errors, log |

`vsaudio_core` is a static library with all of the above; `vsaudio` (the shipped DLL) adds only `api.cpp`.

### Behaviour worth knowing for Phase 2

- **State reporting**: status reflects commands before the render thread applies them (`cmd_seq`/`applied_seq`; same for position via `seek_seq`). Pause on a stopped voice is a no-op; start on a playing voice is a no-op (OpenAL would restart it: Phase 2's `ILoadedSound.Start` must stop first if vanilla code relies on restart).
- **Stop rewinds** to 0. Pause, stop, seek and release of an audible voice fade over 5 ms first; a voice starting from position 0 starts instantly (no fade-in), resuming from elsewhere fades in.
- **Fades** act on the voice gain, run while the voice is paused or stopped, and `set_gain` cancels a running fade (FADE_DONE with the cancelled flag). `VSA_FADE_STOP_WHEN_DONE` covers `FadeOutAndStop`.
- **Streaming**: Ogg assets longer than 20 s (configurable) stream. Looping is done by the worker, so turning looping off late can play up to ~1 s past the loop point before ending. WAV and raw PCM are always decoded.
- **Voices start on block boundaries** (commands apply at block starts). Tests that render in pieces must render whole blocks between voice starts to stay sample-aligned.
- **Offline rendering refills streams synchronously** before each block, so it is deterministic; the worker also services them concurrently under the same lock.
- **Device output**: the engine runs at the device's native rate; channels are native layout clamped to 2/4/6/8 (12 since Phase 3), with front L/R carrying the (non-spatial) Phase 1 mix. Default-device following relies on miniaudio's rerouting; a lost device is reopened by the worker every second, falling back to the default device.
- **Performance leftovers**: the stretched-kernel path (ratio > 1, i.e. most pitched-up sounds) still computes its coefficients with scalar table lookups; it costs ~50 % more per voice than the ratio ≤ 1 path. Worth another look if Phase 2's Steam Audio effects make the budget tight.

### Managed side and tools

- Bindings: `AudioEngine` (assets, voices, devices, offline render, stats, events), `AudioAsset`, `Voice`. Every call leases the engine's `SafeHandle`, so calls racing `Dispose` throw `ObjectDisposedException` instead of touching freed memory.
- Tests that create an engine share the `NativeEngineGroup` xUnit collection (one engine per process).
- SceneLab: `tools/SceneLab` (see `docs/BUILDING.md`). The scenario format is in `tools/SceneLab/Scenario.cs`.
- In-game: `.spatialaudio devices | play <sound> [volume] [pitch] | stop | stats`. The device opens lazily on the first `play` (config `TestOutputDevice` picks one by name); ended voices are released from a 100 ms tick.
- New config keys in `ModConfig/spatialaudio.json`: `ResamplerQuality`, `BlockFrames`, `MaxVoices`, `TestOutputDevice`.

### Dependencies

miniaudio 0.11.25, libogg 1.3.6, libvorbis 1.3.7, pinned in `third_party/deps.json` (hashes verified) and built by `native/cmake/Miniaudio.cmake` and `native/cmake/Vorbis.cmake`. libvorbis's encoder is built for the tests only (they encode their own Ogg files; no binary fixtures in the repo). `THIRD_PARTY_NOTICES.md` in the package now includes the libogg/libvorbis BSD notices.

## Lessons (don't repeat)

- **PowerShell variable names are case-insensitive.** A `$root` in `fetch-deps.ps1` overwrote the script's `$Root` and sent later extractions into the temp folder. The destination guard did not catch it because `$ThirdParty` was computed before the overwrite.
- **Windows `tar`**: use `%SystemRoot%\System32\tar.exe`; a GNU tar from Git for Windows earlier on PATH reads `C:\...` as a remote host.
- **Float sum reductions don't vectorise under strict IEEE semantics**, and `double` → `size_t` has no single x64 instruction. Both showed up in the resampler's hot loop; explicit SIMD and `int64_t` truncation fixed them.
- **Linux CI warnings can be checked locally**: `clang-cl /Zs` with `/clang:-Wall /clang:-Wconversion /clang:-Wdouble-promotion …` (LLVM is installed at `C:\Program Files\LLVM`). Clang flags every implicit float → double conversion, including assignments.
- **Bash `read` with a whitespace `IFS` drops empty fields.** This once made `fetch-deps.sh` `rm -rf` the repo root in the sandbox. Records are NUL-separated and every destination is guarded. Keep both.
- **Out-of-range enum values are undefined behaviour.** Enum-valued ABI fields are `uint32_t`.
- **`AudioData` lives in `Vintagestory.Client.NoObf`** (in VintagestoryLib), not in the API.
- **Mods can't run code at the main menu.** Ownership is per world session (ADR 0006).
- **VS supports a `native/` folder in mods officially.** `Assembly.Location` is valid for zip mods, which are extracted to `Cache/unpack`.
- **Doors and toggle-collision blocks need no hook.** They fire `BlockChanged`/`ChunkDirty` through `BlockEntity.MarkDirty(true)`.
- **The old AcousticLab prototype** (`C:\Projects\VintageStoryAcousticLab`) is abandoned. Don't reuse it.
- **Chris's `VintageStorySurroundSound`** (`C:\Projects\VintageStorySurroundSound`) has useful research in `guides/` (Windows Spatial Audio / 7.1.4 on his AV receiver, an OpenAL Soft `PROPVARIANT` ownership bug) and decompiled game sources in `bin/inspection/` (`Game.cs`, `LoadedSoundNative.cs`, `AudioOpenAl.cs`, `SystemSoundEngine.cs`).

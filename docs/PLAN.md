# Vintage Story Steam Audio — Implementation Plan

Status: draft 2 · 19 Sep 2026 (review decisions applied; Phase 0 built, pending on-machine checks) · Target: Vintage Story 1.22.x (installed build 1.22.7), Steam Audio 4.8.1 (latest release)

## 1. What we are building

A client-side mod that **replaces Vintage Story's audio engine**. Every `ILoadedSound` the game creates is played by our own engine, not by OpenAL. Steam Audio does all spatialisation and acoustic simulation (direct sound, occlusion, transmission, reflections/reverb, pathing), using real world geometry.

### Goals

- Full ownership of playback: decoding, voices, mixing, device output.
- Physically based propagation from voxel geometry and per-block acoustic materials.
- Output modes: headphones (Steam Audio HRTF), stereo speakers, 5.1, 7.1, 7.1.4, and Windows Spatial Audio (Windows Sonic / Dolby Atmos, using both a channel bed and dynamic objects).
- Windows, Linux and macOS (x64 and Apple Silicon) from day one.
- **The game's API contract is honoured, not its acoustics.** Everything code relies on keeps working: `ILoadedSound` lifecycle, categories and their volume sliders, pitch, fades, looping, pause, underwater and glitch hooks. How things *sound* (distance fall-off, reverb, muffling) comes from physical simulation, not from copying OpenAL's curves. We also don't copy vanilla's arbitrary limits, such as the 250-sound cap.
- **Built for enthusiasts.** Quality settings go well past what vanilla targets; every simulation cost is configurable, and there are presets from Low to Ultra.
- Production-grade engineering: no managed code on the real-time audio thread, deterministic tests, measurable budgets, and a clean fallback to vanilla audio.

### Non-goals (for v1)

- Server-side anything. This is a pure client mod.
- New sound assets or re-authoring vanilla sounds.
- GPU acceleration (Radeon Rays / TrueAudio Next). The design leaves room for it, but it is not planned.

### Rules we hold ourselves to

1. **No prototypes that become the product.** Spikes are marked `spike/`, answer one question, and get deleted. Everything else is written as production code with tests.
2. **The real-time audio thread never enters .NET.** GC pauses suspend managed threads, so the device callback, mixer and effects are native code.
3. **Every phase ends with the game playable.** Where an acoustic feature isn't finished, the engine renders plain spatialised sound through Steam Audio, never a hack.
4. **Simulation truth is shown, not guessed.** Debug views show what Steam Audio was given (mesh, materials) and what it produced (per-source parameters, real pathing segments), never raw ray-callback noise.
5. **Every patch target is verified at startup.** If a Harmony target is missing or its signature changed, the mod refuses to take over and vanilla audio keeps working.

## 2. Ground truth: how Vintage Story plays audio (1.22.7)

These findings come from decompiling the installed `VintagestoryLib.dll` (checked 19 Sep 2026). Phase 0 re-verifies them against whatever build is current.

### 2.1 The seam: `ClientPlatformAbstract`

All audio passes through a handful of abstract members on `ClientPlatformAbstract`. They are implemented by the sealed `ClientPlatformWindows`, which is used on **every OS** despite its name.

| Member | Vanilla behaviour | Our replacement |
|---|---|---|
| `StartAudio()` / `StopAudio()` | creates or disposes `AudioOpenAl` (device + context) | start/stop our engine; do not open OpenAL |
| `CreateAudioData(IAsset)` → `NoObf.AudioData` | decodes the whole WAV/OGG (csvorbis) to 16-bit PCM `byte[]` | native decode into a native asset store |
| `CreateAudio(SoundParams, AudioData[, ClientMain])` | `new LoadedSoundNative(...)` | `new SteamAudioSound(...)` (our `ILoadedSound`) |
| `UpdateAudioListener(pos, orient)` | `AL.Listener` with a **flattened** forward vector (y = 0) | ignored; we read the full camera basis ourselves each frame |
| `AvailableAudioDevices`, `CurrentAudioDevice` | OpenAL device list / reopen | our backend's device list / reopen |
| `MasterSoundLevel` | `AL_GAIN` on the listener | engine master gain |
| `AddAudioSettingsWatchers()` | `useHRTFaudio` → recreate context | mapped to our output-mode setting |

No other code in the game touches OpenAL except `LoadedSoundNative`, `AudioOpenAl` and a version log line. So once this surface is owned, **the whole engine is owned**, and there is no parallel OpenAL path to chase.

### 2.2 `LoadedSoundNative`: what is contract and what is just OpenAL

**Not contract (we don't reproduce these):**

- **Distance model:** vanilla uses `AL_EXPONENT_DISTANCE_CLAMPED`. Rolloff is `ln(100)/ln(Range)`, which puts −40 dB at `Range`, and the reference distance is `max(3, √Range − 2)`. Our fall-off is physical instead (§5.3). `Range` becomes a culling and virtualisation hint, and `ReferenceDistance` sets the source's near-field size.
- **The 250-sound cap** in `PlaySoundAt`: patched out. Voice virtualisation (§5.6) handles load.
- **The 24 EFX reverb presets and 40-ray "reverbness" scan:** replaced by simulated reverb.

**Contract (code depends on these):**

- **Gain:** `Volume × category level`. Categories: Sound, Entity, Ambient(+GlitchUnaffected), Weather, Music(+GlitchUnaffected), each with its own ClientSettings slider. Master is listener gain.
- **Pitch:** `Pitch + PitchOffset`, clamped to 0.1–3. We need a variable-rate resampler per voice anyway, because assets have mixed sample rates.
- **Positioning:** `RelativePosition` sounds are head-locked. Positional stereo sounds are *not* spatialised by OpenAL (vanilla logs a warning). With HRTF on, stereo uses `AL_DIRECT_CHANNELS_SOFT = REMIX_UNMATCHED`.
- **Lifecycle:** looping, `PlaybackPosition` get/set, Start/Stop/Pause/Toggle, `IsPlaying`/`IsPaused`/`HasStopped`, `IsReady` (`Loaded == 3`), and deferred start via `AudioMetaData.AddOnLoaded`.
  - `DisposeOnFinish` is cleaned up by `SystemSoundEngine` every 500 ms, after `HasReverbStopped`.
- **Threading:**
  - `FadeTo` runs on the thread pool and calls `SetVolume` off the main thread.
  - The intro music is created with `Platform.CreateAudio` on a thread-pool task.
  - So our `ILoadedSound` and `CreateAudio` **must be thread-safe**.
- **Effects:**
  - `SetLowPassfiltering` uses one shared EFX filter; the game sets 0.06 when underwater.
  - `SetReverb(reverbness)` picks one of 24 EFX reverb slots, driven by `SystemSoundEngine`'s 40-ray "reverbness" scan (stone/metal/ore/brick/ice/ceramic hits within 35 blocks).
  - Our engine ignores these values for world sounds. `SetReverb` is kept only as a hint for when simulation is disabled.
- **Device change:** `LoadedSoundNative.ChangeOutputDevice` disposes every source and recreates it.

### 2.3 Other relevant facts

- ZIP mods are extracted to `VintagestoryData/Cache/unpack/<name>_<hash>/`, and DLL mods are loaded with `Assembly.UnsafeLoadFrom`, so `Assembly.Location` is valid. The mod loader **officially supports a `native/` folder** for unmanaged libraries: it rejects `.dll` files anywhere else except the root. We resolve natives from `native/<rid>/` with `NativeLibrary.SetDllImportResolver`. No installer and no changes to game files are needed.
- Harmony (`0Harmony.dll`) ships with the game.
- The game already references OpenTK and csvorbis; we don't use either.
- **Mod code only runs inside a world.** At the main menu, VS reads mod *infos* only; assemblies are loaded and ModSystems run when a world is joined (`ModLoader.LoadMods` → `RunModPhase`). The intro music starts in `ScreenManager.DoGameInitStage2`, long before that. See §6.3 for what this means for the main menu.
- **Block-change signals are generic.** On the client, `BlockChanged` fires for `MarkBlockDirty`/`MarkBlockModified`, and `ChunkDirty` fires for loads and re-tesselation. Doors, trapdoors and toggle-collision blocks call `BlockEntity.MarkDirty(true)` when their state changes, both on sync and on client prediction. So one pair of event subscriptions covers them; no per-behaviour patches are needed.
- Your VintageStorySurroundSound work established that:
  - Windows Spatial Audio (7.1.4 static bed) works on your AV receiver.
  - OpenAL Soft's WASAPI spatial activation had a `PROPVARIANT` blob-ownership bug. Our own Windows Spatial backend must avoid the same bug class.
  - Dolby Atmos home theater allows 12 static and 20 dynamic objects. Windows Sonic and Atmos for headphones allow 17 static (8.1.4.4) and 128 dynamic.

## 3. Architecture

```text
┌──────────────────────────── Vintage Story process ─────────────────────────────┐
│                                                                                │
│  MANAGED  (VintageStorySteamAudio.dll, net10)                                  │
│  ┌──────────────────┐  ┌───────────────────┐  ┌──────────────────────────────┐ │
│  │ Platform takeover│  │ SteamAudioSound   │  │ World acoustics              │ │
│  │ (Harmony, verify │  │ : ILoadedSound    │  │  chunk snapshots → mesher →  │ │
│  │  & fallback)     │  │ (handle + state)  │  │  material mapping → voxel    │ │
│  └────────┬─────────┘  └─────────┬─────────┘  │  transmission field          │ │
│           │                      │            └──────────────┬───────────────┘ │
│  ┌────────┴──────────────────────┴───────────────────────────┴──────────────┐  │
│  │ Engine client: LibraryImport bindings + SPSC command ring + asset upload │  │
│  └────────────────────────────────┬─────────────────────────────────────────┘  │
│                                   │ C ABI (vsa_*)                              │
│  NATIVE  (vsaudio, C++20)         ▼                                            │
│  ┌──────────────┐  ┌──────────────────────────┐  ┌──────────────────────────┐  │
│  │ Control thr. │→ │ Simulation threads       │→ │ Render (RT) thread       │  │
│  │ commands,    │  │  direct (~30 Hz)         │  │  voices → resample →     │  │
│  │ asset store, │  │  reflections (~10 Hz)    │  │  Steam effects → buses → │  │
│  │ scene edits  │  │  pathing (~10 Hz)        │  │  decode/spatialise →     │  │
│  └──────────────┘  │  scene commit / bakes    │  │  limiter → backend       │  │
│                    └──────────────────────────┘  └────────────┬─────────────┘  │
│  Steam Audio (phonon) · Embree scene · libvorbis              │                │
│  Backends: miniaudio (WASAPI/CoreAudio/PipeWire/Pulse/ALSA) ──┤                │
│            Windows Spatial Audio (ISpatialAudioClient) ───────┘                │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Why a native engine core

- **The GC:** A .NET GC suspends every thread that is running managed code. A managed audio callback would drop out under load, and Vintage Story allocates heavily. The mixer and callback must live on a native thread that never enters the runtime.
- **Steam Audio's shape:** Its threading model expects `iplSimulatorRun*` on worker threads and `ipl*EffectApply` on the audio thread, with parameters handed across without locks. That is natural in C++ and awkward in P/Invoke.
- **Output APIs:** Windows Spatial Audio is COM, and miniaudio is C. Both belong on the native side.
- **The managed side stays thin:** game integration, geometry extraction (it needs the VS API), settings and debug UI.

### 3.2 Threads and data flow

| Thread | Owner | Work | Must never |
|---|---|---|---|
| Game main | VS | `ILoadedSound` calls, listener/camera sample, chunk snapshot copies | block on native locks held by render |
| Geometry workers (2) | managed | greedy meshing, material resolution, voxel transmission field | touch `IBlockAccessor` |
| Control | native | drain command ring, asset lifetime, scene/sub-scene swaps | run DSP |
| Direct sim | native | `iplSimulatorRunDirect` + voxel transmission lookup | allocate per tick |
| Reflection sim | native | `iplSimulatorRunReflections` (source budget) | — |
| Pathing sim | native | `iplSimulatorRunPathing`, background path bakes | — |
| Render (RT) | native | voices, resampling, all `ipl*EffectApply`, mixing, backend write | lock, allocate, log, or touch managed code |

- **Managed → native:** a lock-free SPSC command ring buffer. Commands are POD structs such as `Play`, `SetGain`, `SetPos` and `Fade`.
- **Simulation → render:** triple-buffered parameter blocks per source. The render thread always reads the latest complete set.
- **Native → managed:** telemetry (per-voice state, playback position, meters, sim outputs), read by polling a double-buffered snapshot on the main thread.
- **Voice state for `IsPlaying` and similar:**
  - Stored in atomic per-voice slots, so main-thread queries never wait.
  - Commands issued before the render thread has processed them are reflected immediately in the handle's managed-side state.
  - This is needed because vanilla code calls `Start()` and then reads `IsPlaying` straight away.

## 4. Audio rendering design

Engine format: float32, 48 kHz (or the device rate if it is 44.1 kHz), frame size 256 by default (5.3 ms), configurable to 512.

### 4.1 Per-voice graph

```text
asset PCM ─► variable-rate resampler (rate ratio × pitch; polyphase windowed-sinc,
             click-free loop points) ─► mono/stereo engine-rate frames
   │
   ├─► DIRECT: IPLDirectEffect (physical distance attenuation, air absorption,
   │            occlusion, 3-band transmission) ─► spatialiser (per output mode):
   │              • Headphones: IPLBinauralEffect (HRTF, bilinear) — near/important voices
   │              • Speakers:   IPLPanningEffect to the device layout
   │              • WinSpatial: dynamic object (mono + position) if within object budget
   │              • LOD fallback: IPLAmbisonicsEncode into the world ambisonic bus
   │
   ├─► PATHING: IPLPathEffect (SH coeffs + EQ from pathing sim) ─► world ambisonic bus
   │
   └─► REFLECTIONS (top-N sources): IPLReflectionEffect (HYBRID) via IPLReflectionMixer
                                     ─► world ambisonic bus
```

- **Stereo positional assets:** They are rendered as a two-emitter wide source, each channel slightly offset along the listener's right vector and scaled by distance. This is a documented improvement over vanilla, which doesn't spatialise them. A per-asset override list lets us fall back to vanilla-style head-locked playback.
- **Relative / head-locked sounds** (`RelativePosition = true`, UI, music): these skip simulation and go straight to the non-spatial buses.

### 4.2 Buses and master section

- **World ambisonic bus** (order 2 by default; 3 on the High preset):
  - rotated by the listener orientation with `IPLAmbisonicsRotationEffect`
  - then decoded with `IPLAmbisonicsDecodeEffect` to binaural (HRTF) or to the speaker layout
- **Direct spatialised bus:** already in the output format.
- **Listener reverb bus:** simulated listener-centric reverb (§5.4).
- **Non-spatial buses:** UI/Sound-relative, Music, Weather beds, Ambient beds. These keep vanilla's category volumes.
- **Master:**
  - underwater treatment (driven by the game's existing underwater hooks, rendered physically where possible)
  - glitch pitch jitter (the game's hook, kept)
  - master gain
  - true-peak look-ahead limiter
  - metering

### 4.3 Output modes

| Mode | Direct | Indirect | Backend |
|---|---|---|---|
| Headphones (Steam HRTF) | binaural per voice | ambisonics → binaural | miniaudio, 2 ch |
| Stereo speakers | panning | ambisonics → stereo decode | miniaudio, 2 ch |
| 5.1 / 7.1 | panning | ambisonics → layout decode | miniaudio, 6/8 ch |
| 7.1.4 bed | panning (custom 12-speaker layout) | ambisonics → custom layout | Windows Spatial static bed (miniaudio can't open a 12-channel WASAPI shared stream) |
| Windows Spatial objects (Sonic / Atmos HT / Atmos headphones) | top-K voices as dynamic objects; the rest panned into the bed | ambisonics → 7.1.4 (or 8.1.4.4) bed | Windows Spatial Audio |

- Never run two HRTFs: when Windows Sonic or Atmos-for-headphones is active, Steam Audio sends objects and bed, never binaural output.
- Steam Audio's own HRTF is the default for plain headphones. Custom SOFA HRTFs load via `IPL_HRTFTYPE_SOFA`.
- The object budget comes from `GetMaxDynamicObjectCount()` and is re-queried when the endpoint changes. If it returns 0, we fall back to the bed or to miniaudio.

### 4.4 Assets and decoding

- `CreateAudioData` decodes with **libvorbis** (reference decoder) straight into native memory, not csvorbis into a managed `byte[]`. That avoids double storage.
- Short assets are stored fully decoded (int16 at source rate, the same memory as vanilla).
- Long assets (music, anything over 20 s) are **streamed**: decoded in chunks on the control thread into ring buffers ahead of the render cursor.
- `AudioMetaData` stays valid for game code:
  - Its `Loaded` state machine and `AddOnLoaded` continue to work.
  - `Pcm` is replaced with a zero-length array. The only non-audio reader is a memory-statistics line, which we patch to report native bytes.
- WAV assets go through the same pipeline.

## 5. Acoustic simulation design

### 5.1 Geometry: voxels → Steam Audio scene

- **Scene type:** `IPL_SCENETYPE_EMBREE` where the Embree device can be created, otherwise `IPL_SCENETYPE_DEFAULT` (Steam Audio's own ray tracer). The engine probes this at start-up and reports it. Phase 0 found that the macOS library is universal (x86_64 + arm64); whether its arm64 slice supports Embree is confirmed by the CI smoke test on an Apple Silicon runner.
- **Structure:**
  - One top-level `IPLScene` holds one `IPLInstancedMesh` per loaded chunk (32³) inside the acoustic radius.
  - Each instance is a sub-scene containing one `IPLStaticMesh`.
  - A chunk edit rebuilds only that chunk's sub-scene; after that, the top-level `iplSceneCommit` just re-indexes instances.
- **Meshing:**
  - Surfaces are generated only at **boundaries between different acoustic materials**, including solid ↔ air.
  - Coplanar same-material faces are greedy-merged.
  - Faces are emitted only where the neighbour is loaded; unloaded neighbours are treated as solid to avoid leaks.
- **Block shape fidelity:**
  - Full cubes: faces.
  - Partial blocks (slabs, stairs, fences, doors, trapdoors, chiseled/microblocks): their **collision boxes**, meshed as box surfaces.
  - Leaves: modelled as surfaces with high transmission and scattering.
  - Liquids: water surfaces with their own material. The underwater listener is handled separately.
- **Dynamic block state:**
  - Doors and trapdoors are block-entity behaviours whose collision changes without a block-ID change.
  - We hook the behaviour's open/close to dirty the chunk. This is verified in Phase 0.
  - Other block-entity state changes go through `BlockChanged`/`ChunkDirty`.
- **Pipeline:**
  - main thread: copy the chunk's block IDs and the relevant block-entity states into pooled arrays (≤ 0.2 ms per chunk)
  - geometry worker: mesh + material IDs + voxel transmission field
  - native control thread: build the sub-scene and swap it in between simulation ticks
- **Budget and LOD:**
  - A full-resolution radius (default 2 chunks) and a coarse ring beyond it (2³-block super-voxels, majority material) up to 4 chunks.
  - Vertical extent is capped to the listener's ±2 chunks.
  - Triangle counts are measured on real worlds in Phase 4 and enforced by preset.
- **Debugging:** `iplSceneSaveOBJ` export plus an in-game wireframe of *exactly* the submitted triangles, coloured by material.

### 5.2 Acoustic materials

- Base table keyed by `EnumBlockMaterial` (Stone, Ore, Brick, Ceramic, Metal, Wood, Soil, Gravel, Sand, Snow, Ice, Glass, Leaves, Plant, Cloth, Liquid, Lava, Mantle, …).
- Each entry has 3-band absorption, scattering and 3-band transmission. Values start from Steam Audio's preset materials.
- Overrides by block code (wildcards) live in a JSON asset, `assets/vsteamaudio/config/acousticmaterials.json`. Other mods can extend it through the asset system or a block attribute (`attributes.acoustics`).
- Material IDs are dense `uint16`. The native side holds the `IPLMaterial` array per sub-scene.

### 5.3 Direct sound: occlusion, transmission, distance

- **Occlusion:**
  - Steam Audio volumetric occlusion (`IPL_OCCLUSIONTYPE_VOLUMETRIC`), 16–32 samples.
  - The source radius comes from the sound category (entity ≈ 0.5 m, block ≈ 0.5 m, ambient emitters larger).
- **Transmission (thickness-aware):**
  - Steam Audio's transmission multiplies per-*surface* coefficients. With boundary-only meshing, 1 block and 6 blocks of stone would both be "two surfaces".
  - So we compute transmission ourselves: a voxel DDA from listener to source over the snapshot, accumulating **actual path length inside each material** and applying per-band attenuation, `T_b = Π exp(−α_b(material) × length)`.
  - The result is written into `IPLDirectEffectParams.transmission[3]` with `IPL_TRANSMISSIONTYPE_FREQDEPENDENT`.
  - Glass, wood doors, one stone block and a mountain then sound different.
  - This runs on the direct-sim thread against the latest voxel field. The field is a compact per-chunk material grid, shared read-only.
- **Distance attenuation:** physical inverse-distance fall-off (Steam Audio's default model). The minimum distance comes from the source's size (`ReferenceDistance` or the category default), so nearby sounds don't blow up.
  - Vanilla assets were balanced for OpenAL's curve. We rebalance with per-category trims and one global "world scale" setting, tuned by ear against test scenes. We don't copy the old curve.
  - `Range` doesn't shape loudness. It only feeds virtualisation (§5.6).
- **Air absorption:** Steam Audio's default model.
- **Updates:** every direct-sim tick (30 Hz) for all audible voices. Effects interpolate parameters per frame.

### 5.4 Reflections and reverb

- **Listener reverb (always on):**
  - A reflections simulation with the source at the listener (Steam Audio's "reverb" usage) gives a room/cave response that follows the space you're in.
  - It feeds a shared convolution reverb that all world sounds send to, weighted by their direct-path energy.
  - This replaces vanilla's 40-ray "reverbness" and its 24 fixed EFX presets.
- **Per-source reflections (budgeted):**
  - The top N sources by importance (loudness × proximity × category weight × persistence) get their own real-time reflection simulation.
  - `IPL_REFLECTIONEFFECTTYPE_HYBRID` gives convolution early reflections plus parametric late tail. The effects are mixed through `IPLReflectionMixer` into the ambisonic bus, so the cost is one convolution for all sources.
  - The rest get listener reverb only.
- **Quality presets:**
  - Rays 2048–8192
  - Bounces 8–32
  - IR duration 1.0–2.5 s
  - Ambisonic order 1–2
  - N sources 4–32
  - Irradiance minimum distance 1 m
- **Open-sky handling:** rays escaping above the loaded vertical extent count as sky, which is fully absorptive. Otherwise an open field would reverberate off the snapshot's edges.

### 5.5 Pathing (sound around corners and through doorways)

Steam Audio's pathing needs **baked** probe-to-probe visibility data (`iplPathBakerBake`). The old prototype skipped the bake, which is why its pathing was silent. Our design:

1. **Probe generation per region** (64 × 64 blocks by chunk-aligned columns): `iplProbeArrayGenerateProbes` with `IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR`, which naturally follows voxel floors. Spacing is 2–3 m and height 1.6 m.
2. **Background bake** per region on the pathing thread (cancellable). Several region probe batches are active at once through `iplSimulatorAddProbeBatch`.
3. **Edits between bakes:** `enableValidation` + `findAlternatePaths` keep paths correct when blocks change. A dirty region re-bakes after a hysteresis delay.
4. **Rendering:** `IPLPathEffect` into the ambisonic bus, with EQ from the deviation model. Real path segments are shown through the pathing visualisation callback.
5. **Decision gate (Phase 7, first task):** a spike measures bake time and memory for a 64 × 64 × 64 region with typical caves and buildings.
   - If a region bakes in ≤ 2 s on one worker thread, we ship baked pathing.
   - If not, we use the designed alternative: **voxel portal propagation**. This is a flood fill over the air/transmission field from the source, finding the dominant entry direction and path length at the listener. It is converted to SH coefficients and EQ, and fed to `IPLPathEffect` with the same rendering path.
   - Either way the renderer and tests are the same.

### 5.6 Source management

- **No fixed voice cap.** Every playing sound has a voice, but only *audible* ones are rendered.
  - A voice below the audibility threshold for its estimated level becomes **virtual**: it keeps its playback position but costs no DSP. It becomes real again when it rises above the threshold, with hysteresis.
  - The number of real voices is a quality setting (for example 64 on Low, 512+ on Ultra), not a hard-coded rule.
- Every real positional voice is a Steam Audio `IPLSource`, registered with the simulator.
- An importance score decides reflection and pathing eligibility, and dynamic-object assignment in Windows Spatial mode.
- Moving sources (entities) are updated through `SetPosition`, as in vanilla. Optionally, sounds can be bound to entity motion (the tracking problem your Surround mod solved); that is Phase 8.

## 6. Vintage Story integration

### 6.1 Takeover and verification

- On load, the mod resolves every patch target (§2.1) by name **and signature**, then checks the native engine initialises and opens a device.
- Only if all of that succeeds does it apply the Harmony patches.
- Any failure leaves vanilla audio untouched and shows a clear in-game notice. The notice explains the version mismatch or missing native library.

### 6.2 `SteamAudioSound : ILoadedSound`

- Implements the full interface contract (§2.2). It is thread-safe: an immutable handle ID plus atomics, with commands going to the ring buffer.
- `FadeTo`/`FadeIn`/`FadeOut` are native sample-accurate gain ramps. The callbacks are raised on the main thread, as in vanilla.
- `SetReverb` records a hint. It is used only when simulated reverb is disabled or unavailable, through a parametric reverb bus.
- `SetLowPassfiltering` maps to a per-voice low-pass on the direct and indirect sends.

### 6.3 Lifecycle and main menu

VS gives mod code no hook at the main menu (§2.3), so ownership is **per world session**:

1. **Taking over** (`StartPre`, the earliest client phase, before any world sound exists):
   - verify all patch targets
   - start the engine
   - patch the platform
   - migrate any live `LoadedSoundNative`, such as the fading intro music, at its playback position
   - close the OpenAL context
2. **During the session**, every sound, including music and UI, plays on our engine.
3. **Handing back** (`Dispose` on world exit):
   - stop our voices
   - unpatch
   - re-open vanilla OpenAL through the original `StartAudio()`
   - the main menu then plays through vanilla again

Main-menu music and UI clicks therefore stay vanilla. The only way to own them would be to change how the game starts (a launcher or patched game files), which we rule out: this ships as a normal ModDB mod. ADR 0006 records the decision; we revisit it if VS adds a main-menu mod phase.

- Device changes, the settings-menu device list and hot-plug all go through our backend. A lost device switches to the default device, with a notice.

### 6.4 Settings and UI

- A settings dialog (with ConfigLib integration if installed) covers:
  - output mode and device
  - HRTF selection
  - quality preset (Low / Balanced / High / Ultra)
  - per-feature toggles (reflections, pathing, transmission)
  - the object budget
- The vanilla HRTF checkbox maps onto Headphones mode.
- The debug overlay (F10) has three layers:
  - **Scene truth:** the submitted mesh and materials, chunk states and triangle counts.
  - **Acoustic truth:** per source, the direct gain, occlusion, transmission bands, reflection energy, pathing direction and real path segments.
  - **Engine truth:** voice count, CPU per thread, render callback time histogram, xruns, latency.
- Recording: capture the master bus to WAV, plus a JSON trace of sources and parameters, for offline comparison.

### 6.5 Compatibility

- **VintageStorySurroundSound** overlaps completely. The mods are declared incompatible, and ours detects it and refuses to take over with an explanation. Its entity tracking and weather routing ideas can be ported later.
- **Mods that call OpenAL directly** (e.g. voice chat): we detect known mod IDs. In non-spatial output modes a compatibility mode keeps a secondary OpenAL context open on the same endpoint.
- **Mods that play sounds through the API** work unchanged, because they all go through `CreateAudio`.

## 7. Repository layout

```text
VintageStorySteamAudio/
  docs/           PLAN.md, BUILDING.md, adr/NNNN-*.md, investigations/
  native/         C++20 engine (CMake + presets: win-x64, linux-x64, linux-x64-asan, osx)
    include/vsaudio.h        stable C ABI (the only managed/native interface)
    src/core/                errors, logging; later: command ring, asset store, voices, resampler, buses, limiter
    src/steam/               Steam Audio context, RAII handles, self-test; later: scene manager, simulator threads, effects
    src/backend/             (Phase 1) miniaudio backend, (Phase 3) Windows Spatial backend
    src/decode/              (Phase 1) libvorbis/wav decoders, streaming
    tests/                   doctest unit tests against the public C ABI (+ a C-compile check of the header)
    cmake/                   platform/RID, Steam Audio import, warnings
  src/
    VintageStorySteamAudio/          the mod (net10, client)
      Native/                         LibraryImport bindings, native resolver, AudioEngine wrapper
      Platform/                       integration-point catalogue + verifier; (Phase 2) Harmony takeover
      Config/  Diagnostics/
      (later) Sounds/  World/  UI/
  tests/
    VintageStorySteamAudio.Tests/    xUnit v3: ABI layout, verifier, type names, game + native integration
  tools/
    VsaDoctor/                       checks a game install + native engine without launching the game
    (Phase 1) SceneLab/              headless CLI: test voxel scene → render WAV + metrics
  third_party/                       deps.json (pinned URLs + SHA-256), VERSIONS.md
  scripts/                           fetch-deps.{ps1,sh}, build.ps1
  deploy.ps1                         build + install into VintagestoryData/Mods
  .github/workflows/ci.yml           native matrix (win/linux/mac + sanitizers), managed build/test, packaging
```

## 8. Build, packaging, licences

- Native code: CMake presets per RID. CI builds all RIDs and produces `native/<rid>/{vsaudio,phonon}.*`, and the managed build packages the mod zip. Symbols are kept as release artifacts.
- Pinned dependencies, with hashes recorded in `third_party/VERSIONS.md`:

  | Dependency | Licence |
  |---|---|
  | Steam Audio 4.8.1 | Apache-2.0 (Embree inside) |
  | miniaudio | public domain / MIT-0 |
  | libogg / libvorbis | BSD |
  | doctest | MIT |

  `THIRD_PARTY_NOTICES` ships in the mod.
- **Distribution: one normal mod zip** with natives for every platform under `native/<rid>/`. No installer and no game-file changes.
  - ModDB offers one file per release and VS's mod manager has no per-OS downloads, so a single cross-platform zip is the right format.
  - Measured size: about 51 MB compressed (the three `phonon` binaries are ~119 MB uncompressed).
- `scripts/build.ps1` and `deploy.ps1` (PowerShell 7, cross-platform) build, test, package and install into `VintagestoryData/Mods`, optionally restarting the game.

## 9. Testing strategy

| Level | What | How |
|---|---|---|
| Native unit | ring buffer, resampler (THD+N, aliasing), loop points, fades, limiter, command semantics | doctest, sanitizers in CI |
| Golden render | known scene + source → WAV; assert metrics: loudness vs inverse-distance law, ILD/ITD-derived azimuth, RT60 estimate, transmission band ratios | SceneLab, tolerance-based, runs in CI |
| Managed unit | mesher output vs expected surfaces, material resolution, DDA path lengths, `ILoadedSound` state machine | xUnit with synthetic voxel volumes |
| In-game acceptance | canonical scenes in `testworlds/` | scripted checklist per phase + recordings |
| Performance | CPU per thread, callback p99, xruns, memory, triangles | budgets enforced in CI (SceneLab) and via the in-game HUD |

**Canonical scenes:**

- open field
- small stone room
- large cave
- a pane of glass
- a closed wood door
- stone walls 1 / 3 / 6 blocks thick
- the "goat in a room, doorway to a hallway" test
- a waterfall approached from a cave
- underwater
- 200 simultaneous sounds

**Budgets (Balanced preset, mid-range desktop):**

- Render thread ≤ 25 % of the callback period at p99, with zero xruns in a 30-minute session.
- Simulation threads ≤ 1.5 cores in total.
- Main thread ≤ 0.5 ms per frame on average.
- Native memory ≤ 300 MB, excluding decoded assets.

## 10. Roadmap

Every phase ends with the game fully playable and the phase's tests green.

| Phase | Content | Exit criteria |
|---|---|---|
| **0 · Foundations** ✅ | repo, CI for 3 OSes, CMake/native skeleton, C ABI + LibraryImport bindings, native loading from `Cache/unpack`, Steam Audio + Embree smoke test per RID; re-verify §2 against the current build; main-menu hook investigation; door state-change hook; ADRs 1–5 | CI green on all RIDs; mod loads natively in-game on Win (Linux/mac via CI + one manual test); seam verified |
| **1 · Engine core** | miniaudio backend, device enumeration/hot-plug, RT render thread, command ring, asset store + libvorbis + streaming, voices, resampler, buses, master limiter, telemetry, SceneLab skeleton | golden tests for resampling/looping/fades; SceneLab renders WAVs; no xruns under a synthetic 256-voice load |
| **2 · Engine takeover** | Harmony takeover + verification + fallback, `SteamAudioSound` (full API contract), physical distance model, voice virtualisation, 250-cap removal, categories, pitch, underwater/glitch, live-sound migration and hand-back, settings device list; Steam Audio direct effect + panning/binaural (first real Steam Audio output) | full play session with no missing, stuck or misbehaving sounds; category mix rebalanced by ear; OpenAL context closed in-world and restored at the menu |
| **3 · Output modes** | HRTF (+SOFA), stereo, 5.1/7.1 panning, ambisonic world bus + decode, 7.1.4 custom layout, Windows Spatial backend (bed + dynamic objects, budget, fallback) | per-mode direction tests (front/back/above/below, turning, looking up/down); receiver shows Atmos with correct height |
| **4 · World geometry** | material table + overrides, chunk snapshots, greedy mesher, collision-box shapes, door hooks, instanced-mesh scene manager, LOD ring, OBJ export, scene-truth overlay | mesh matches world in test scenes; edit → scene latency ≤ 250 ms; triangle/memory budgets met |
| **5 · Direct simulation** | simulator threads, sources, volumetric occlusion, voxel transmission, air absorption, importance/LOD, acoustic-truth overlay | thickness test matrix passes (glass < wood < 1 stone < 3 stone < 6 stone, per band); smooth updates while moving |
| **6 · Reflections & reverb** | listener reverb, per-source hybrid reflections via reflection mixer, sky handling, presets; retire vanilla reverbness | RT60 tracks room/cave size in golden tests; CPU budget met |
| **7 · Pathing** | bake-cost spike (decision gate) → baked-region pathing or voxel portal fallback; path effect; real path visualisation | goat/doorway scene: perceived direction follows the doorway, validated by recording + azimuth metric |
| **8 · Release hardening** | entity-bound source tracking, stereo-asset overrides, compatibility modes, presets tuning, ConfigLib, docs, packaging for ModDB | 3-OS release candidate; 2-hour soak without xruns or leaks |

### Phase 0 status (19 Sep 2026)

Done:

- the repo, the build (CMake presets, `build.ps1`/`deploy.ps1`) and CI for three platforms
- pinned, hash-checked dependencies
- the C ABI, `LibraryImport` bindings and the native resolver
- the Steam Audio context with Embree/built-in ray tracer selection and a native self-test
- the integration-point verifier and the `VsaDoctor` tool
- the investigations and ADRs 0001–0006

Verified on Linux x64 in the build sandbox:

- native tests pass under GCC, Clang and ASan/UBSan
- the managed tests pass
- `VsaDoctor` reports 28/28 integration points against the installed 1.22.7 game DLLs, and the self-test passes through the managed layer

Still to confirm on real machines:

- an in-game load on Windows (`.steamaudio status`)
- the first CI run: the MSVC and macOS builds, Embree on Apple Silicon, and the server-package reference assemblies

Findings: [investigations/phase0.md](investigations/phase0.md).

## 11. Key risks

| Risk | Mitigation |
|---|---|
| Pathing bakes too slow for a dynamic voxel world | decision gate + fully designed voxel-portal fallback that uses the same renderer |
| Triangle counts in rugged terrain | boundary-only meshing, greedy merge, LOD ring, measured budgets per preset |
| VS updates change internals | signature-verified patch targets, refuse-and-fallback, decompile diff checklist per VS release |
| Windows Spatial activation quirks (seen with OpenAL Soft) | own backend with correct PROPVARIANT ownership, activation diagnostics, automatic bed/miniaudio fallback |
| Third-party mods using OpenAL directly | detection + compatibility context in non-spatial modes |
| macOS Apple Silicon support gaps (Embree, notarisation of native libs) | Phase 0 smoke test; `IPL_SCENETYPE_DEFAULT` fallback |
| CPU cost of HRTF per voice with many sources | per-voice binaural only for top voices; the rest go through the ambisonic bus with one binaural decode |

## 12. Decisions from review (19 Sep 2026)

1. **Music and UI move to our engine.** They are routed through non-spatial buses while in a world (see §6.3 for the main menu).
2. **The target is enthusiasts.** The Balanced preset is calibrated to a 6-core desktop. Low, High and Ultra go down and well beyond it, and every cost is configurable.
3. **Packaging is a single native mod zip** (§8). No installer.
4. **We don't try to sound like vanilla.** Fall-off, reverb and occlusion are physical. Vanilla's arbitrary caps are removed. The game's API contract is kept.

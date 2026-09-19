# Handover (19 Sep 2026)

This is the state of the work when it was handed to Claude Code. Previous sessions ran in a cloud sandbox and could only read and write this repo, not run commands on Windows.

## Where things stand

### Phase 0 (foundations) is complete

Verified in the cloud sandbox (Linux):

- Native tests pass under GCC, Clang and ASan/UBSan.
- `VsaDoctor` reports 28/28 game integration points against the 1.22.7 DLLs, and the self-test passes through the managed layer.
- The build produces the mod zip.

Verified by Chris on Windows (`pwsh ./scripts/build.ps1`, first run):

- The native engine builds cleanly with MSVC (`/W4 /WX`).
- Native tests pass, including the Steam Audio self-test.
- Natives install to `artifacts/native/win-x64`.
- The mod and VsaDoctor build.

Verified by Claude Code on Windows (full `pwsh ./scripts/build.ps1`):

- Managed tests compile and pass (25/25). The `<Using Include="Xunit" />` fix had not actually landed in the csproj; it has now.
- VsaDoctor against the real 1.22.7 client install: 28/28 integration points, self-test passed.
- Packaging produces `artifacts/vssteamaudio_0.1.0.zip` (win-x64 natives).
- `deploy.ps1` installs the zip into `%APPDATA%\VintagestoryData\Mods`.

Not yet run:

- an in-game load (`.steamaudio status` / `.steamaudio targets` in chat)
- CI (`.github/workflows/ci.yml`, never pushed)

### Repo housekeeping (done)

- Git repo initialised with a baseline commit.
- `ci.yml` was on disk as `.github/workflow/ci.yml` (singular); moved to `.github/workflows/`.
- `scripts/dev-runner.ps1` and its `.gitignore` entry deleted.

### Other audio mods in the Mods folder

`%APPDATA%\VintagestoryData\Mods` also has `vintagestoryacousticlab_0.1.0.zip` and `vintagestorysurroundsound_1.2.3.zip`. Both may patch the same audio methods. Disable them when testing this mod. Phase 2's takeover should also detect foreign Harmony patches on our targets and refuse to take over (nothing checks for this yet).

## Immediate next steps

1. Start the game, join a world, and run `.steamaudio status`.
   - Expect: engine running, ray tracer Embree, self-test passed, 28/28 integration points, "Audio takeover: ready".
   - The game log is under `%APPDATA%\VintagestoryData\Logs` (client-main.log).
2. If the repo gets pushed to GitHub, check the first CI run. It should settle the macOS build, Embree on Apple Silicon (macos-14 runner) and whether the dedicated-server package works as reference assemblies (`VS_VERSION` 1.22.7).
3. Start Phase 1.

## Phase 1 (engine core): design already worked out

Goal from PLAN.md §10:

- the miniaudio backend, device enumeration and hot-plug
- the real-time render thread and the command ring
- the asset store, libvorbis and streaming
- voices, the resampler, buses, the master limiter and telemetry
- the SceneLab skeleton

Exit criteria:

- golden tests for resampling, looping and fades
- SceneLab renders WAVs
- no xruns under a synthetic 256-voice load

No Phase 1 code has been written. These are the decisions already made.

### Dependencies (verified downloads)

| Dep | Version | URL | SHA-256 |
|---|---|---|---|
| miniaudio | 0.11.25 (latest tag; single header, compile with `MINIAUDIO_IMPLEMENTATION` in one TU) | `https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h` | `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89` |
| libogg | 1.3.6 | `https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.gz` | `83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638` |
| libvorbis | 1.3.7 | `https://github.com/xiph/vorbis/releases/download/v1.3.7/libvorbis-1.3.7.tar.gz` | `0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab` |

- **Fetch scripts:** `fetch-deps.ps1` and `fetch-deps.sh` need a new `tar.gz` kind. `tar -xzf` works on Windows 10+ and Unix. Keep the destination guard.
- **Building ogg and vorbis:** don't `add_subdirectory` their CMake projects, because vorbis's `find_package(Ogg)` fights it. Build static `vsa_ogg` and `vsa_vorbis`/`vorbisfile` targets directly from their source lists in `native/cmake/Vorbis.cmake`.
  - Generate `ogg/config_types.h` from `config_types.h.in`.
  - Exclude vorbis's non-library files: `psytune.c`, `barkmel.c`, `tone.c`, and the `vorbisenc.c` encoder.

### Threads

| Thread | Work |
|---|---|
| Device callback (miniaudio, real-time priority) | The render thread. It pulls fixed engine blocks (default 256 frames) through a FIFO adapter, because devices request arbitrary frame counts. |
| Worker (one non-real-time native thread, woken every ~2 ms) | Stream refills; draining the render→worker rings (retired voices, events, real-time log records); device-lost/rerouted handling and reopening; deferred frees. |
| API threads (any) | Push commands into a lock-free SPSC ring. Producers are serialised by a mutex that the render thread never touches, so any thread can call. |

This matters because the game calls `SetVolume` from the thread pool during fades and creates the intro music off the main thread.

### Voices

- **Slots:** a preallocated slot table with capacity from config (default 4096; this is a storage bound, not an audibility cap). Each slot holds its resampler history.
- **Handles:** `uint64 = generation << 32 | slot`, and the API validates the generation.
- **Life cycle:**
  1. Create: the API takes a slot from a free list (mutex), initialises it, then sends `Activate`.
  2. Release: the render thread retires the slot and pushes it to the retired ring.
  3. The worker returns the slot and drops the asset reference.
- **State readable without blocking:** atomics per slot (state, source position). There is also a `cmd_seq`/`applied_seq` pair, so a state request the render thread hasn't applied yet is still reported. This is needed because vanilla calls `Start()` and immediately reads `IsPlaying`.
- **Commands:** start, pause, stop, set gain (with a smoothing ramp, default 5 ms), set pitch, set looping, seek, fade (dB-linear, which matches vanilla's geometric `FadeTo`, with a completion event token), set bus gain, set master gain, release.
- **Declicking:** pause and stop use a ~5 ms fade.
- **Events** (fade done, voice ended, stream underrun, device changed): render → SPSC → worker → a locked queue that the API polls.

### Assets

- **Creation:** `vsa_asset_create(desc)` decodes Ogg or WAV (or accepts raw s16 PCM) on the caller's thread.
- **Storage:** short assets are stored as interleaved int16 at the source rate, the same memory footprint as vanilla.
- **Streaming:** long assets (auto above ~20 s, or forced) keep the encoded bytes. Each streaming voice gets a vorbisfile decoder over memory plus an SPSC float ring (~1 s) that the worker refills.
  - Looping and seeking happen on the worker. A seek generation is used; the render thread outputs silence until the new data arrives.
  - When the ring runs dry, the render thread outputs silence and counts an underrun.
- **Lifetime:** reference counted (the owner holds one reference, each voice another). Final deletion always happens off the render thread.
- **Channels:** 1–2 channels only, like vanilla; reject more with a clear error.

### Resampler

- **Method:** bandlimited interpolation (Julius O. Smith's method).
  - A Kaiser-windowed sinc prototype table with ~512 samples per zero crossing, linearly interpolated.
  - When the ratio (source rate / engine rate × pitch) exceeds 1, the kernel is scaled by the cutoff, so taps grow with the ratio. Cap the ratio at 8.
- **Quality setting** (zero crossings per side): Low 4, Medium 8 (default), High 16.
- **Fast path:** ratio == 1.
- **Golden tests:** passband flatness, SNR/THD+N on sines, and alias rejection when pitching up near Nyquist.

### Mixing and master

- **Buses:** Sound, Entity, Ambient, Weather, Music. These match vanilla's category volume sources (`LoadedSoundNative.GlobalVolume`), with the GlitchUnaffected variants folding into their parent.
- **Voice routing:** each voice → its bus → master gain → limiter → output channels.
- **Phase 1 has no spatialisation.** Mono goes equal-power to front L/R; stereo goes to L/R. Steam Audio spatialisation arrives in Phase 2.
- **Limiter:** true-peak look-ahead.
  - 4× oversampled peak estimate using a short polyphase FIR.
  - ~2 ms look-ahead with a sliding-window minimum of target gain and boxcar smoothing, then exponential release.
  - Channels linked; ceiling −1 dBTP.

### Output

- **Enumeration:** `vsa_device_enumerate` returns fixed-size records: `name[256]`, an opaque `id[512]` (`static_assert(sizeof(ma_device_id) <= 512)`), and is-default.
- **Opening:** `vsa_output_open` takes kind DEVICE or NONE, a device id (null means follow the default), and a channel count (0 = native; supported 2, 4, 6 or 8).
- **Sample rate:** the engine runs at the device's native rate; offline runs at the rate requested.
- **miniaudio context:** thread priority real-time.
- **Hot-plug:** notifications `stopped` and `rerouted` set atomics, and the worker reopens the device (follow-default). Never uninit the device from its own callback.
- **Offline rendering:** `vsa_engine_render_offline(float*, frames)`, only when the output is NONE. It is deterministic and runs on the calling thread, which acts as the render thread. It backs tests and SceneLab.
- **Stats:** `vsa_engine_get_stats` returns blocks rendered, render time avg/max, overloads (render time > block period), stream underruns, active voices, device rate/channels/period, and peak limiter gain reduction since the last read.

### Tests to add

- Unit tests for the ring, resampler, loop points, fades, limiter and command semantics.
- A **zero-allocation render test** on Linux and macOS: override global `operator new` in the test executable (ELF/Mach-O interposition reaches into libvsaudio) and assert that `render_offline` allocates nothing.
- A 256-voice load test measuring render time against the block period.

### Managed side and tools

- **Bindings:** C# wrappers for assets, voices, devices, stats and events.
- **SceneLab** (`tools/SceneLab`, C#): reads a JSON scenario (assets, voice timings and parameters), renders offline to WAV and writes metrics JSON.
- **In-game test commands:** `.steamaudio devices` and `.steamaudio play <asset>`. `play` decodes a game `.ogg` via `api.Assets` and plays it through our own device alongside vanilla OpenAL (WASAPI shared mode), so Phase 1 can be heard before the Phase 2 takeover.

## Lessons from earlier sessions (don't repeat)

- **Bash `read` with a whitespace `IFS` drops empty fields.** This shifted columns in the first `fetch-deps.sh` and made it `rm -rf` the repo root in the sandbox. Records are now NUL-separated and every destination is guarded. Keep both.
- **Out-of-range enum values are undefined behaviour.** UBSan caught a C enum type receiving an out-of-range value from the ABI, which is why enum fields are `uint32_t`.
- **`AudioData` lives in `Vintagestory.Client.NoObf`** (in VintagestoryLib), not in the API.
- **Mods can't run code at the main menu.** Ownership is per world session (ADR 0006).
- **VS supports a `native/` folder in mods officially.** `Assembly.Location` is valid for zip mods, which are extracted to `Cache/unpack`.
- **Doors and toggle-collision blocks need no hook.** They fire `BlockChanged`/`ChunkDirty` through `BlockEntity.MarkDirty(true)`.
- **The old AcousticLab prototype** (`C:\Projects\VintageStoryAcousticLab`) is abandoned. Don't reuse it.
- **Chris's `VintageStorySurroundSound`** (`C:\Projects\VintageStorySurroundSound`) has useful research in `guides/`, notably Windows Spatial Audio / 7.1.4 on his AV receiver and an OpenAL Soft `PROPVARIANT` ownership bug. Its `bin/inspection/` folder holds decompiled game sources (`Game.cs`, `LoadedSoundNative.cs`, `AudioOpenAl.cs`, `SystemSoundEngine.cs`), which are handy references.

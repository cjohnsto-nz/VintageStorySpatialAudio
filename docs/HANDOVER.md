# Handover (19 Sep 2026)

State of the work at the end of the first Claude Code session on Chris's Windows machine. Earlier sessions ran in a cloud sandbox that could only edit files.

## Where things stand

### Phase 0 (foundations): complete and verified on Windows

- Native, managed, VsaDoctor (28/28 integration points against the 1.22.7 client) and packaging all pass through `pwsh ./scripts/build.ps1`.
- In-game load confirmed by Chris: `.steamaudio status` showed engine running with Embree, self-test passed, 28/28, "Audio takeover: ready"; clean shutdown ("engine destroyed").
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

- **In game**: `.steamaudio devices`, `.steamaudio play effect/woodswitch` (plays through our engine alongside vanilla OpenAL), `.steamaudio stats`, `.steamaudio stop`. The mod was not redeployed after Phase 1; run `pwsh ./deploy.ps1 -StopGame` first.
- **CI** (never pushed): GCC `-Werror` (only clang-cl was used locally), macOS universal build (the SIMD header picks SSE2/NEON per slice), ASan/UBSan over the new code, and whether miniaudio's null backend makes the Linux device test open a device (the test accepts either outcome).

### Other audio mods in the Mods folder

`%APPDATA%\VintagestoryData\Mods` also has `vintagestoryacousticlab_0.1.0.zip` and `vintagestorysurroundsound_1.2.3.zip`. Both may patch the same audio methods. Disable them when testing this mod. Phase 2's takeover should also detect foreign Harmony patches on our targets and refuse to take over (nothing checks for this yet).

## Immediate next steps

1. Deploy (`pwsh ./deploy.ps1 -StopGame`), join a world, try the four test commands above, and listen: `.steamaudio play` of a short effect, a looping ambience and a music track (music streams). Check `client-main.log` for the "Test output opened" line and any `[native]` warnings.
2. Push to GitHub and get CI green on all three platforms (see "Not yet verified").
3. Merge `phase1-engine-core`, then start Phase 2 (engine takeover, PLAN.md §10).

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
- **Device output**: the engine runs at the device's native rate; channels are native layout clamped to 2/4/6/8, with front L/R carrying the (non-spatial) Phase 1 mix. Default-device following relies on miniaudio's rerouting; a lost device is reopened by the worker every second, falling back to the default device.
- **Performance leftovers**: the stretched-kernel path (ratio > 1, i.e. most pitched-up sounds) still computes its coefficients with scalar table lookups; it costs ~50 % more per voice than the ratio ≤ 1 path. Worth another look if Phase 2's Steam Audio effects make the budget tight.

### Managed side and tools

- Bindings: `AudioEngine` (assets, voices, devices, offline render, stats, events), `AudioAsset`, `Voice`. Every call leases the engine's `SafeHandle`, so calls racing `Dispose` throw `ObjectDisposedException` instead of touching freed memory.
- Tests that create an engine share the `NativeEngineGroup` xUnit collection (one engine per process).
- SceneLab: `tools/SceneLab` (see `docs/BUILDING.md`). The scenario format is in `tools/SceneLab/Scenario.cs`.
- In-game: `.steamaudio devices | play <sound> [volume] [pitch] | stop | stats`. The device opens lazily on the first `play` (config `TestOutputDevice` picks one by name); ended voices are released from a 100 ms tick.
- New config keys in `ModConfig/vssteamaudio.json`: `ResamplerQuality`, `BlockFrames`, `MaxVoices`, `TestOutputDevice`.

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

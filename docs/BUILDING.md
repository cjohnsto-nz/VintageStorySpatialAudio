# Building

## Prerequisites

| | Windows | Linux | macOS |
|---|---|---|---|
| C++ | Visual Studio 2022 (Desktop C++ workload) | GCC 13+ or Clang 17+, Ninja | Xcode command-line tools, Ninja |
| CMake | 3.25+ | 3.25+ | 3.25+ |
| .NET SDK | 10.0 | 10.0 | 10.0 |
| PowerShell | 7+ (`pwsh`) | 7+ | 7+ |
| Game | Vintage Story 1.22.x installed | same | same |

Tell the build where the game is if it isn't in the default location:

```powershell
$env:VINTAGE_STORY = 'C:\Users\you\AppData\Roaming\Vintagestory'   # folder containing VintagestoryAPI.dll
```

## One command

```powershell
./scripts/build.ps1 [-Configuration Debug|Release] [-SkipTests]
```

This runs the following steps in order:

1. `scripts/fetch-deps.ps1` downloads Steam Audio 4.8.1, miniaudio, libogg, libvorbis and doctest, checks their SHA-256 hashes and extracts them into `third_party/`.
2. Native: `cmake --preset <rid>`, then build, `ctest` (`vsaudio_tests` through the C ABI, `vsaudio_core_tests` on engine internals), and install into `artifacts/native/<rid>/`.
3. Managed: build the solution (and run tests unless `-SkipTests`).
4. `VsaDoctor` verifies every game integration point against your installed game and runs the native self-test.
5. SceneLab renders every scenario in `tools/SceneLab/scenarios/` to `artifacts/scenelab/` (WAV + metrics JSON) and fails the build if a scenario's expectations are not met (unless `-SkipTests`).
6. Packaging: `artifacts/mod/` (a drop-in mod folder) and `artifacts/vssteamaudio_<version>.zip`.

`./deploy.ps1` runs the build (Debug, tests off by default) and copies the zip into `VintagestoryData/Mods`. Add `-StopGame` and/or `-LaunchGame` as needed.

## Native only

```bash
./scripts/fetch-deps.sh            # or fetch-deps.ps1
cd native
cmake --preset linux-x64           # win-x64 | linux-x64 | osx
cmake --build --preset linux-x64-debug
ctest --preset linux-x64-debug
cmake --preset linux-x64-asan && cmake --build --preset linux-x64-asan && ctest --preset linux-x64-asan   # sanitizers
```

## SceneLab

Renders a JSON scenario offline through the engine, without the game:

```powershell
dotnet tools/SceneLab/bin/Release/net10.0/SceneLab.dll tools/SceneLab/scenarios/phase1-smoke.json --out out
```

A scenario lists assets (files relative to the scenario, or generated `tone` / `noise`), voices (bus, gain, pitch, loop, start time, `count` copies with a `pitchSpread`) and timed `actions` (start, pause, stop, release, gain, pitch, loop, seek, fade). `expect` sets pass/fail limits: `maxUnderruns`, `maxOverloads`, `maxPeakDbfs`, `minRmsDbfs`, `maxP99Load` (p99 block render time over the block period). The metrics JSON has per-channel peak/RMS, block render times, underruns, limiter reduction and the engine events with their times.

## Checking a game update

When Vintage Story updates, run this before anything else:

```powershell
dotnet tools/VsaDoctor/bin/Release/net10.0/VsaDoctor.dll --game <install> --targets-only
```

Every failing entry names the member, what was expected and what exists now (including types that moved namespace). Update `src/VintageStorySteamAudio/Platform/AudioPatchTargets.cs` first, then the code that uses the target.

## CI

`.github/workflows/ci.yml` does three things:

- Builds and tests the native engine on Windows, Linux (plus ASan/UBSan) and macOS (Apple Silicon, universal binary).
- Builds and tests the managed code against the public dedicated-server package for reference assemblies, then renders the SceneLab scenarios (WAVs uploaded as `scenelab-renders`).
- Uploads a single cross-platform mod zip.

The game-integration checks need the *client* assemblies, so they run locally (step 4 above), not in CI.

## Dependency changes

Edit `third_party/deps.json` (URL + SHA-256) and `third_party/VERSIONS.md` together. The fetch scripts refuse to run on a hash mismatch, and refuse any destination outside `third_party/`.

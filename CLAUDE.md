# CLAUDE.md

Guidance for Claude Code in this repo. The current state and next steps are in [docs/HANDOVER.md](docs/HANDOVER.md). Read it first.

## What this is

A Vintage Story client mod that replaces the game's audio engine with a native engine built on Steam Audio 4.8.1.

- The plan is in `docs/PLAN.md`.
- The decisions are in `docs/adr/` (ADRs 0001–0009).
- The evidence behind them is in `docs/investigations/phase0.md`.

## Build and test (Windows, PowerShell 7)

Always use `pwsh`. Windows PowerShell 5.1 can't run the scripts (they carry `#Requires -Version 7.0`).

```powershell
pwsh ./scripts/build.ps1                      # deps -> native build+tests -> managed build+tests -> VsaDoctor -> zip
pwsh ./scripts/build.ps1 -SkipTests           # faster; skips the test project entirely
pwsh ./deploy.ps1 -StopGame                   # Debug build + install zip into %APPDATA%\VintagestoryData\Mods
cd native; cmake --preset win-x64; cmake --build --preset win-x64-debug; ctest --preset win-x64-debug
dotnet tools/VsaDoctor/bin/Release/net10.0/VsaDoctor.dll --native artifacts/native/win-x64
```

- **Game location:** the game is at `C:\Users\chris\AppData\Roaming\Vintagestory` (1.22.7), which is the default path the build detects. Set `VINTAGE_STORY` to point anywhere else.
- **Toolchain:** Visual Studio Build Tools 2022 with the C++ workload, CMake, and the .NET 10 SDK are installed.

## Rules (from PLAN.md, non-negotiable)

- **The real-time audio thread never enters .NET, allocates, locks or logs.** The engine core is native C++20 (`native/`). C# only integrates with the game.
- **The C ABI (`native/include/vsaudio.h`) has strict conventions.**
  - Every struct starts with `struct_size`, and there is an ABI version to bump on incompatible changes.
  - Enum-valued struct fields are `uint32_t`.
  - Nothing throws across the boundary.
  - Keep `src/VintageStorySteamAudio/Native/VsaNative.cs` and `tests/.../NativeLayoutTests.cs` in lockstep with the header.
- **Steam Audio lifetime:** every `Add` has a matching `Remove` + `Commit` before `Release` (a LeakSanitizer finding). Use RAII guards.
- **Game hooks go through `Platform/AudioPatchTargets.cs`.** Add the hook there first; `VsaDoctor` and the startup verifier check it. If verification fails, the mod refuses to take over and vanilla audio stays in charge.
- **Dependencies are pinned in `third_party/deps.json` (URL + SHA-256), with `VERSIONS.md` kept in sync.** The fetch scripts refuse hash mismatches and any destination outside `third_party/`.
- **No prototypes that become the product.** Spikes live under `spike/` and get deleted. Everything else is production code with tests.
- **We don't imitate vanilla acoustics.** Fall-off, reverb and occlusion are physical, and vanilla's arbitrary caps (such as the 250-sound limit) are removed. The game's API contract (`ILoadedSound` behaviour, categories, fades, and so on) is kept.
- **Quality settings target enthusiasts:** presets from Low to Ultra, and every simulation cost is configurable.

## Code style

- **C++:** warnings-as-errors (`/W4 /WX`, `-Wall -Wextra -Wconversion ... -Werror`), clean under GCC, Clang and MSVC. Linux CI also runs ASan/UBSan (`linux-x64-asan` preset).
- **C#:** .NET 10, nullable, `TreatWarningsAsErrors`, `AnalysisLevel latest-recommended`. Use `CultureInfo.InvariantCulture` for formatting. Tests use xUnit v3; the global `using Xunit` is declared in the test csproj.

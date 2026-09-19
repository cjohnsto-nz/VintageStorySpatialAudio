# Steam Audio for Vintage Story

A client mod that replaces Vintage Story's audio engine with a native engine built on [Steam Audio](https://valvesoftware.github.io/steam-audio/). Sound propagation (occlusion, thickness-aware transmission, reflections, reverb and pathing around corners) is simulated from the actual world geometry. Output modes are HRTF headphones, stereo, 5.1/7.1, 7.1.4 and Windows Spatial Audio (Windows Sonic / Dolby Atmos).

**Status:** Phase 0 (foundations). The mod loads its native engine, self-tests Steam Audio and verifies every game integration point, but it does not take over audio yet. See [docs/PLAN.md](docs/PLAN.md).

## Layout

| Path | What |
|---|---|
| `native/` | `vsaudio`, the C++20 engine (CMake). Public C ABI in `native/include/vsaudio.h` |
| `src/VintageStorySteamAudio/` | the Vintage Story mod (C#, .NET 10) |
| `tests/VintageStorySteamAudio.Tests/` | managed unit + integration tests (xUnit v3) |
| `tools/VsaDoctor/` | checks a game install and the native engine without launching the game |
| `third_party/` | pinned dependency manifest (`deps.json`); fetched content is not committed |
| `scripts/` | `fetch-deps`, `build` |
| `docs/` | plan, ADRs, investigations |

## Quick start

```powershell
# Windows, PowerShell 7. Needs VS 2022 C++ tools, CMake >= 3.25, .NET 10 SDK and the game installed.
./scripts/build.ps1          # deps -> native build + tests -> managed build + tests -> VsaDoctor -> zip
./deploy.ps1 -LaunchGame     # build (Debug) and install into VintagestoryData/Mods
```

In game, type `.steamaudio status` or `.steamaudio targets`.

See [docs/BUILDING.md](docs/BUILDING.md) for Linux, macOS and details.

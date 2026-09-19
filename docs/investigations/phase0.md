# Phase 0 findings (19 Sep 2026)

These are the Phase 0 investigation results. Each one is backed by evidence you can re-check: a decompiled source excerpt, a tool run, or a test.

Game build examined: **Vintage Story 1.22.7** (`VintagestoryLib 1.22.7.0`), Windows install.

## 1. The audio seam is complete and verified

`tools/VsaDoctor` checked all 26 integration points in `AudioPatchTargets` and both structural invariants against the installed `VintagestoryLib.dll`. The result was 28/28 passing.

- `ClientPlatformWindows` is the **only** concrete `ClientPlatformAbstract`, and it is used on every OS.
- `ClientPlatformAbstract` declares no audio members besides the ones we replace.

Two corrections to PLAN.md came out of the tool:

- `AudioData` is `Vintagestory.Client.NoObf.AudioData`, which lives in `VintagestoryLib`, not the API. `CreateAudioData`, `CreateAudio` and `ScreenManager.soundAudioData` all use that type.
- `ClientPlatformWindows` sits in `Vintagestory.Client.NoObf`.

After a game update, re-run `VsaDoctor --game <install>` before touching anything else.

## 2. Mods cannot run code at the main menu

- **What happens at the menu:** `ScreenManager.loadMods` only calls `ModLoader.LoadModInfos` and `DisableAndVerify`. No mod assembly is loaded.
- **When mod code first runs:** assemblies are loaded (`ModContainer.LoadAssembly` → `Assembly.UnsafeLoadFrom`) and ModSystems run (`RunModPhase(Pre/Start/AssetsLoaded/…)`) only when a world starts (`ClientMain` → `PreStartMods`/`StartMods`).
- **The intro music starts earlier:** `ScreenManager.DoGameInitStage2` creates it with `Platform.CreateAudio` on a thread-pool task, well before any mod code runs.

**Consequence:** we own audio per world session and hand it back at the menu (ADR 0006).

## 3. Native libraries ship inside a normal mod

- **Where zip mods go:** zip mods are extracted to `Cache/unpack/<file>_<hash>/`, and `ModContainer.FolderPath` points there.
- **The `native/` folder:** `ModContainer` rejects `.dll` files outside the mod root, with the message *"If you need to ship unmanaged dlls, put them in the native/ folder"*. So `native/<rid>/` is an officially supported location.
- **Finding the natives:** DLL mods load via `Assembly.UnsafeLoadFrom`, so `Assembly.Location` points into the unpacked folder. `NativeLibraryResolver` uses that path.

**Consequence:** no installer and no game-file changes are needed. The resolver was exercised end-to-end on Linux by `VsaDoctor` and by the managed integration test.

## 4. Doors and other block-entity shape changes need no special hooks

- **What doors do:** `BEBehaviorDoor.ToggleDoorState` and `FromTreeAttributes` (server sync) call `Blockentity.MarkDirty(true)`. Trapdoors and `BEBehaviorToggleCollisionBox` work the same way.
- **What that triggers:** on the client, `MarkDirty(true)` leads to `MarkBlockDirty`/`MarkBlockModified`, which fire `BlockChanged`, and `MarkChunkDirty` fires `ChunkDirty(MarkedDirty)`.
- **How collision boxes are exposed:** `BlockToggleCollisionBox.GetCollisionBoxes` reads the block entity's state. Doors expose `ColSelBoxes` through the block's collision-box query.

**Consequence:** Phase 4 subscribes to `BlockChanged` + `ChunkDirty` and snapshots `GetCollisionBoxes(accessor, pos)` for blocks that have a block entity.

The door source came from your local `vssurvivalmod` checkout, which is from mid-2025. Phase 4 re-checks it against 1.22.

## 5. Steam Audio 4.8.1 on our platforms

- **Latest release:** 4.8.1 is still the newest release (Feb 2025). SDK zip SHA-256: `4a0aa5ec…1d5449`.
- **Linux x64:** Embree 4.04 initialises. The self-test passes with both Embree and Steam Audio's built-in ray tracer (natively and through the managed layer).
- **macOS:** `libphonon.dylib` is universal (x86_64 + arm64), has install name `@rpath/libphonon.dylib`, and needs macOS 11.0 or later. Whether the arm64 slice supports Embree can't be settled by inspection. The CI job on `macos-14` (Apple Silicon) answers it, and the engine falls back to the built-in tracer automatically if needed.
- **Windows x64:** `phonon.dll` plus `phonon.lib`. `GPUUtilities.dll` and `TrueAudioNext.dll` are only needed for AMD GPU features, so we don't ship them.
- **Linux arm64:** Steam Audio has no build. That matches Vintage Story's own platform support.

## 6. Steam Audio lifetime rule (found with LeakSanitizer)

Releasing an `IPLStaticMesh` that is still added to an Embree scene leaks its Embree geometry. The same risk applies to sources still added to a simulator.

**Rule:** always `Remove` + `Commit` before `Release`.

The self-test uses RAII attachment guards (`MeshAttachment`, `SourceAttachment`), and the Phase 4 scene manager must use the same pattern. CI runs the native tests under ASan/UBSan on Linux.

## 7. ABI hygiene (found with UBSan)

Loading an out-of-range value through a C enum type is undefined behaviour, and the managed side can send any 32-bit value. So every enum-valued field in `vsaudio.h` structs is declared `uint32_t` and validated natively. The header documents this rule.

## 8. Package size

The three `phonon` binaries total about 119 MB uncompressed and **about 51 MB zipped** (measured). One cross-platform mod zip is the distribution format.

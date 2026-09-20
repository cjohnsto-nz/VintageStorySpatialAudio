# ADR 0019: The Linux and macOS libraries are a second mod

**Status:** Accepted, 21 Sep 2026.

## Context

The Vintage Story mod database takes files up to 40 MB. The release package with all three
platforms is 54 MB, and almost all of it is Steam Audio's own library, already compressed:

| | Compressed |
|---|---|
| Windows `phonon.dll` | 19.3 MB |
| Linux `libphonon.so` (already stripped) | 17.0 MB |
| macOS `libphonon.dylib` (x64 + arm64) | 15.0 MB |
| Everything of ours | about 1 MB |

No arrangement of all three fits. Dropping one macOS architecture still leaves 46 MB.

One mod with a release per platform does not work either: a release is one file with its own
version number, the game's version format has no room for a platform, and the in-game updater
installs the latest release — so whichever platform was uploaded last would be handed to everyone.

Downloading the library at first run was not considered: a mod that fetches and loads executable
code is not something players should be asked to trust, and it breaks offline installs.

## Decision

- **Two mods.** `spatialaudio` is the mod, with the Windows libraries (about 20 MB): the large
  majority of players install one thing, as before. `spatialaudiounix` ("Spatial Audio: Linux and
  macOS natives", about 33 MB) holds only `native/linux-x64` and `native/osx`. It depends on
  `spatialaudio` at exactly its own version, and both are built from the same CI run.
- **The mod looks in its own folder first, then in the pack's**
  (`NativeLibraryResolver.FindNativeDirectory`). It finds the pack by its assembly among those
  loaded, and takes `native/<rid>/` from beside it.
- **The pack is a code mod with one mod system that does nothing.** The game unpacks a zip to disk
  only for a code mod, and a library can only be loaded from disk.
- **A pack of another version is refused**, not loaded: the message says to update both. The ABI
  version check in the engine is the second line of defence.
- **Without the pack on Linux or macOS the mod stands down** the way it does for any other failed
  start-up check — vanilla audio carries on — and says which mod to install, in the log and once
  in the chat.
- **`scripts/build.ps1 -Split` makes the two zips and fails if either is over 40 MB.** Without it
  there is one zip with whatever was built, which is what a developer on any platform installs.

## Consequences

- Linux and macOS players install two mods. The mod page says so.
- There are 7 MB of headroom in the pack. If Steam Audio grows past it, the pack splits into one
  per platform without touching the mod: the resolver only needs a second assembly name to look for.
- Every release is two uploads, of the same version. `NativePackTests` fails the build if the two
  `modinfo.json` versions, or the pack's dependency on the mod, disagree.

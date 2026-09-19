# ADR 0006: Session-scoped audio ownership

**Status:** Accepted, 19 Sep 2026

## Context

Phase 0 established that Vintage Story loads mod assemblies and runs ModSystems only when a world starts. At the main menu it reads mod infos only. The intro music is created before any mod code runs.

Owning the menu's audio would need code running earlier, which means a launcher or patched game files. We ruled that out: the mod ships as a normal ModDB zip with no installer.

## Decision

Ownership is per world session.

1. **Takeover** (earliest client phase):
   - verify all integration points; refuse the takeover if any fail
   - start the engine
   - patch the platform
   - migrate live vanilla sounds, such as the intro music, at their playback position
   - dispose the vanilla sounds and close the OpenAL context
2. **Hand-back** (`Dispose` on world exit):
   - stop our voices
   - unpatch
   - re-open OpenAL through the original `StartAudio()`

## Consequences

- Main-menu music and UI clicks stay vanilla.
- The takeover and hand-back paths are tested explicitly, including repeated join/leave cycles in one process. The native engine allows one instance at a time, and it must be recreated cleanly.
- If a future game version adds a main-menu mod phase, this ADR is revisited.

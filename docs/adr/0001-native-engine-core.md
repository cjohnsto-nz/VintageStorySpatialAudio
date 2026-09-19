# ADR 0001: Native engine core

**Status:** Accepted, 19 Sep 2026

## Context

The mod replaces the game's audio engine. The audio callback must deliver a buffer every 5–10 ms, forever, without dropouts.

Vintage Story allocates heavily on the managed heap. A .NET garbage collection suspends every thread that is executing managed code. Steam Audio's effects (`ipl*EffectApply`) are designed to run on the audio thread. Its simulations (`iplSimulatorRun*`) run on worker threads, with parameters handed across without locks.

## Decision

- Mixing, voices, resampling, Steam Audio effects and simulation, and device I/O live in a C++20 shared library, `vsaudio`, built with CMake for win-x64, linux-x64 and macOS (universal).
- The managed mod talks to it through a small, versioned C ABI (`native/include/vsaudio.h`), using `LibraryImport` bindings.
- Commands cross in one direction through a lock-free ring buffer (from Phase 1); telemetry is read back by polling.
- The real-time render thread never calls managed code, never allocates, and never takes locks.

## Consequences

- We build and ship three native binaries, and CI must cover all three platforms (done in Phase 0).
- The ABI is a contract with strict rules (see the header): `struct_size`, an ABI version, no exceptions across the boundary, and enums as `uint32_t` fields. Managed layout tests and native size checks enforce them.
- Game integration stays in C#, where the Vintage Story API and Harmony are.

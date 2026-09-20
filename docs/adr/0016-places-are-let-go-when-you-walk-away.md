# ADR 0016: A place is let go when the listener walks away from it

**Status:** Accepted, 20 Sep 2026. Amends ADR 0012.

## Context

ADR 0012 holds a place for 30 s after its last sound, so that a sound repeated at one spot — an
anvil struck again and again — always uses the same, converged simulation instead of starting
cold. That is right when the listener stays put.

Phase 8's profiling caught what it costs when they do not. Walking a route with two audible
sounds, the engine kept **ten places live**, and the reflection simulation was the largest
remaining cost in the mod: 26.8 ms per tick across three threads, most of Steam Audio's 75 % of
a core. The places being simulated were the ones left behind — ground the listener had walked
away from, whose reflections could not be heard from where they now were. The simulation budget
is shared round-robin among live places, so those also stole refreshes from the places that were
still audible.

## Decision

- **An idle place beyond 48 m from the listener is let go at once**, without waiting out the
  30 s. "Idle" is unchanged: no sound is sending to it now.
- Within 48 m the 30 s hold stands, so ADR 0012's reason for holding — the repeated sound at one
  spot — is untouched.

## Consequences

- Walking leaves no trail of simulated places: the slots and the simulation budget follow the
  listener. `test_reflections.cpp` holds it — a place 14 m behind is kept, the same place at
  64 m is let go the moment the listener is that far.
- A sound that finished more than 48 m away and is repeated once the listener returns starts
  from a cold simulation, as a sound at a new place does (ADR 0012: its effects run muted until
  the first result, about 10–20 ms). At that distance its reflections were inaudible anyway.
- 48 m is a judgement, not a measurement: far enough that a sound's reflections are lost under
  the direct sound's own distance attenuation, near enough to cover a room and its surroundings.
  It is not configurable; if the quality presets ever need it, it belongs with them.

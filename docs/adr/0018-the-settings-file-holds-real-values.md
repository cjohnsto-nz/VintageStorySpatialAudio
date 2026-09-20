# ADR 0018: The settings file holds real values, not zeros standing for defaults

**Status:** Accepted, 20 Sep 2026.

## Context

Every tunable in `spatialaudio.json` used `0` to mean "use the default", resolved in three
layers: an explicit value, else the named quality preset, else the engine's own default. The
file a player opened was mostly zeros, and nothing in it said what the engine would actually do —
`"ReflectionRateHz": 0` is 10 Hz, `"PathingRangeBlocks": 0` is 64 blocks, and the only way to
find out was the startup line in the log or a debug command.

The convention earned its place once already: when the pathing box changed from 96 to 64 blocks,
every existing config picked the new value up with no migration. `ReflectionGain` is not
zero-based — 0 is a real gain — and when its default changed from 1 to 0.1, files kept the old
value and it took a migration step to correct.

But the mod is aimed at people who will tune it, and a settings file that will not tell you its
settings is the wrong trade. Chris, tuning reflections, could not tell what the rate was.

## Decision

- **Every setting is written out in full.** On load, any value still unset is filled in with what
  the engine would use and the file is stored back, so it always states what will happen.
- **The defaults come from the engine, not from a copy in C#.** `vsa_get_default_config` (ABI 14)
  resolves a zeroed config through the same code an engine is built with and hands back every
  value, the ones that depend on the machine included (the reflection thread count). There is one
  source of truth.
- **`.spatialaudio config`** prints the file's path and the settings that matter; **`.spatialaudio
  config reset`** puts every setting back to its default and rewrites the file, which is how a
  player picks up defaults that changed in a new version.
- **A zero in the file still means "the default"**, so a file written by an older version, or
  hand-edited back to zero, keeps working and is filled in on the next load.
- **The quality preset still drives the reflection numbers.** The preset a file's numbers came
  from is recorded (`ReflectionQualityApplied`); change `ReflectionQuality` and they are worked
  out again from the new preset. Values already in a file from before this change are the
  player's and are never overwritten.

## Consequences

- The file is self-documenting, and `.spatialaudio config` answers "what is it doing" without the
  log.
- **A default that changes no longer reaches an existing config on its own.** That is the cost of
  the trade, and `.spatialaudio config reset` is the answer; release notes should say when a
  default worth adopting has changed. `Migrate` (from `ConfigVersion`) stays for defaults that
  must change under a player who never runs reset.
- Editing a single value still works and still wins, as before.
- The engine's defaults and the managed presets can no longer drift apart, because the managed
  side asks rather than remembers.

# ADR 0021: The native pack is matched by ABI, not by version

**Status:** Accepted, 21 Sep 2026. Amends ADR 0019.

## Context

ADR 0019 split the Linux and macOS libraries into a second mod, `spatialaudiounix`, because all
three platforms together are 54 MB against the mod database's 40 MB. It also decided that **a pack
of another version is refused**, with the engine's ABI check as "the second line of defence", and
`NativePackTests` failed the build unless both `modinfo.json` versions and the pack's dependency
were the same string.

Two releases in, that rule is doing the wrong work. The pack contains `libvsaudio` and `libphonon`
and no code of ours — it is a carrier for files. Its mod version therefore says nothing about
whether it fits: it tracks the mod's release number, which moves for reasons that have nothing to
do with the libraries. Release 0.1.1 is the case in point. It changes C# only, and `native/` has
not moved since the commit the 0.1.0 pack was built from, so that pack's libraries are built from
the sources 0.1.1 wants, at the same ABI. (Not the same bytes — the build is not reproducible — but
nothing in them differs.) Under ADR 0019 every player on those platforms would have to download
34 MB again for that, and every future C#-only release would do the same.

What actually has to agree is the **ABI** — `VSA_ABI_VERSION` in `native/include/vsaudio.h`
against `VsaNative.AbiVersion` — and that is already checked, against the library itself rather
than against a version string, in `AudioEngine.Create`.

## Decision

- **`FindNativeDirectory` does not look at versions.** The mod's own `native/` folder wins; failing
  that, the pack's is used whatever version the pack is.
- **The ABI check is the only line of defence**, which is what it was always able to be. Its message
  now names the pack when the libraries came from it, so the reader is told what to update.
- **The pack's `modinfo.json` dependency stays a floor.** Vintage Story's `ModDependency.Version`
  is documented as a minimum, so `"spatialaudio": "0.1.1"` reads "needs the mod at 0.1.1 or above".
  The floor is `NativeLibraryResolver.FirstVersionAcceptingAnyPack`: below it the mod is one that
  still demands its own version, and would refuse this pack.
- **A pack is rebuilt and re-released only when the libraries change**, not with every mod release.

## Consequences

- A C#-only release does not oblige anyone to re-download 34 MB. Players on Linux and macOS keep
  the pack they have until the native side moves.
- The two `modinfo.json` versions are allowed to differ, so `NativePackTests` no longer demands
  they match. What it checks instead is that the pack's floor lies between
  `FirstVersionAcceptingAnyPack` and the mod's own version, and that the pack does not claim to be
  newer than the mod it was built for.
- A mismatched pack is now caught later — at engine creation rather than at library lookup — and by
  the ABI rather than by a version string. That is a better test, not just a later one: it catches
  a hand-copied or stale library that happens to sit in a correctly-versioned pack, which the old
  check waved through.
- The ABI must be bumped honestly. It always had to be, but it was backed up by the version rule
  and now is not: a native change that alters the C ABI without bumping `VSA_ABI_VERSION` will load
  an incompatible library and misbehave rather than refuse. The rule in CLAUDE.md stands —
  every incompatible change to `vsaudio.h` bumps the ABI version.
- ADR 0019's reason for the split, and its packaging, are untouched.

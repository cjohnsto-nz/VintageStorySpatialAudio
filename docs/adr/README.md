# Architecture decision records

Each ADR records one decision: the context, the choice and its consequences. ADRs are not edited after acceptance; a later ADR supersedes an earlier one.

| # | Decision | Status |
|---|---|---|
| [0001](0001-native-engine-core.md) | The engine core is native C++; the real-time thread never enters .NET | Accepted |
| [0002](0002-output-backends.md) | Output goes through miniaudio and our own Windows Spatial Audio backend, not OpenAL | Accepted |
| [0003](0003-voxel-scene-representation.md) | World geometry goes to Steam Audio as per-chunk instanced meshes on Embree | Accepted |
| [0004](0004-voxel-transmission.md) | Transmission through solids is computed from voxel path length, not Steam Audio surface counts | Accepted |
| [0005](0005-pathing-decision-gate.md) | Pathing: baked Steam Audio pathing, gated on measured bake cost, with a designed fallback | Accepted |
| [0006](0006-session-scoped-ownership.md) | Audio ownership is per world session; the main menu stays vanilla | Accepted |
| [0007](0007-native-chunk-meshing.md) | Chunks cross the ABI as voxel snapshots and are meshed natively, with a floating origin | Accepted |
| [0008](0008-transmission-crossings-and-bulk.md) | Transmission is a loss per material crossing plus a loss per metre (amends 0004) | Accepted |
| [0009](0009-reflections-rendering.md) | Reflections: a slot pool, convolved early reflections and our own diffuse tail (amends PLAN §5.4) | Accepted |
| [0010](0010-reverb-follows-what-reaches-you.md) | Reverb follows what reaches the listener; short sounds share spots (amends 0009) | Superseded by 0011 |
| [0011](0011-one-listener-reverb.md) | One listener reverb that every sound feeds by what reaches the listener (supersedes 0010) | Superseded by 0012 |
| [0012](0012-every-sound-simulated-from-its-place.md) | Every sound is simulated from its place; no other path (supersedes 0010 and 0011) | Accepted |
| [0013](0013-baked-pathing.md) | Pathing is baked: the ADR 0005 gate, measured (0.5 s per 64³ region) | Accepted |
| [0014](0014-one-rolling-probe-batch.md) | Pathing runs in one probe batch that rolls with the listener (amends PLAN §5.5 and 0013) | Accepted |
| [0015](0015-the-pathing-bake-is-budgeted.md) | The pathing bake is bounded by a probe budget, and abandoned when the listener moves on (amends 0013 and 0014) | Accepted |
| [0016](0016-places-are-let-go-when-you-walk-away.md) | A place is let go when the listener walks away from it (amends 0012) | Accepted |
| [0017](0017-a-stale-bake-is-discarded-not-cancelled.md) | A stale bake is discarded, not cancelled: Steam Audio's cancel is unusable (corrects 0015) | Accepted |
| [0018](0018-the-settings-file-holds-real-values.md) | The settings file holds real values, not zeros standing for defaults | Accepted |
| [0019](0019-the-linux-and-macos-libraries-are-a-second-mod.md) | The Linux and macOS libraries are a second mod: the mod database takes 40 MB | Accepted |
| [0015](0015-multichannel-beds.md) | Multichannel beds (5.1 weather) play from their speakers; binaurally through a head-locked bus | Accepted |

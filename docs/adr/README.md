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

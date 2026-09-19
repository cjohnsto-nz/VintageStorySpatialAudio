# ADR 0007: Chunks are meshed natively from voxel snapshots

**Status:** Accepted, 19 Sep 2026

## Context

The plan (§3.2) put greedy meshing and the voxel transmission field in two managed geometry worker threads, with the native side only building sub-scenes. But the native side needs the voxel material grid anyway: Phase 5's transmission (ADR 0004) runs a voxel DDA on the native direct-simulation thread. Meshing in managed code would mean building the same grid twice, a managed/native mesh hand-off per chunk, and tests split across two languages.

Vintage Story's block coordinates are also around 500 000, where a 32-bit float resolves only about 6 cm, and every Steam Audio position is a float.

## Decision

- **Snapshots cross the ABI, not meshes.** On the main thread, the managed side copies a chunk into 32³ acoustic material ids (`uint16`), plus the collision boxes of partial blocks. It hands them over with `vsa_scene_set_chunk`. That is the only managed work per chunk.
- **The native scene thread does the rest.** It meshes the chunk with a boundary-only greedy mesher, builds the chunk's Steam Audio sub-scene and instances it in the top-level scene. It keeps the grid for the transmission DDA.
- **Surfaces follow material kinds.** Each material has a kind: air, liquid, porous or solid. A surface is emitted where a denser kind meets a more open one. Buried interfaces between two solids produce no surface: nothing reflects there, and transmission through them comes from the grid.
- **Neighbours are re-meshed only when needed.** A chunk's border faces depend on its neighbours. A newly arrived or removed chunk re-meshes its loaded neighbours; an edit re-meshes only the neighbours whose shared layer changed.
- **Floating origin.** Scene coordinates are relative to an integer block origin near the listener (`vsa_scene_set_origin`). Each chunk instance is placed at its offset from the origin, and moving the origin updates the instance transforms. The managed side sends listener and voice positions relative to the same origin.
- **The debug views read the real meshes.** `vsa_scene_get_chunk_mesh` returns each chunk's mesh exactly as submitted to Steam Audio, and `vsa_scene_save_obj` exports the scene. The in-game wireframe therefore shows the truth, not a re-derivation.

## Consequences

- There is one grid and one mesh per chunk, both native, and both unit-tested in C++. A real Steam Audio occlusion query checks that geometry lands where the world puts it.
- Managed per-chunk cost is a copy and a table lookup per cell. The mesher costs about 1 ms per terrain chunk (Release) on the scene thread.
- Managed code must keep positions origin-relative and re-send them when the origin moves.
- Supersedes the "geometry workers (managed)" row of PLAN.md §3.2.

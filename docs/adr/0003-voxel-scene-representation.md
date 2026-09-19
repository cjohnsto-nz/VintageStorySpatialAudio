# ADR 0003: Voxel scene representation

**Status:** Accepted, 19 Sep 2026

## Context

The prototype answered Steam Audio's ray queries with managed callbacks (`IPL_SCENETYPE_CUSTOM`). That was slow, ruled out Embree and multithreading, and gave every surface the same material. The world is voxels in 32³ chunks, edited continuously.

## Decision

- **Scene structure:** one top-level Steam Audio scene. Each chunk becomes one `IPLInstancedMesh`, whose sub-scene holds an `IPLStaticMesh`.
- **Meshing:** surfaces are generated only at boundaries between different acoustic materials, with coplanar faces greedy-merged. Partial blocks use their collision boxes.
- **Edits:** changing a chunk rebuilds only its sub-scene.
- **Ray tracer:** `IPL_SCENETYPE_EMBREE` where the Embree device can be created, otherwise `IPL_SCENETYPE_DEFAULT`, chosen once at start-up (`vsa_engine_info.active_ray_tracer`).
- **Lifetime rule (from the Phase 0 LeakSanitizer finding):** every `Add` has a matching `Remove` + `Commit` before `Release`, enforced by RAII guards.

## Consequences

- Updates cost one chunk plus a top-level re-index, not a rebuild of the whole world.
- Triangle budgets depend on terrain and must be measured on real worlds (Phase 4), with a coarse LOD ring beyond the full-detail radius.

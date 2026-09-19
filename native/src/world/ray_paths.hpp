#pragma once

#include "world/transmission.hpp"

#include <cstdint>
#include <vector>

namespace vsa::world {

/// One leg of a traced sound path (scene coordinates).
struct RaySegment {
    float from[3] = {};
    float to[3] = {};
    /// Mid-band energy left on arrival at `to` (1 when leaving the origin).
    float energy = 1.0f;
    /// 0 for the leg leaving the origin.
    uint32_t bounce = 0;
    /// What `to` is on; 0 if the leg ended in the open (nothing within reach).
    uint16_t material = 0;
};

/// Sound paths from `origin` for the debugging view of the reflections (Phase 6): `rays`
/// directions spread evenly over the sphere, each followed through up to `bounces` reflections off
/// the voxel world, specular or (by each material's scattering) diffuse, losing each surface's
/// absorption. Deterministic, so the drawing does not flicker. Not what Steam Audio traces
/// (it has its own rays and the meshes), but the same surfaces and materials: it shows where
/// sound goes and how quickly it dies.
[[nodiscard]] std::vector<RaySegment> trace_paths(const VoxelView& view, const float origin[3], uint32_t rays,
                                                  uint32_t bounces, float max_distance, std::size_t max_segments);

}  // namespace vsa::world

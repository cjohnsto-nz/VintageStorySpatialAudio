#pragma once

#include "world/voxel.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace vsa::world {

/// Triangles for one chunk, in chunk-local block units (0..32 per axis).
struct ChunkMesh {
    std::vector<float> vertices;      // x, y, z per vertex
    std::vector<int32_t> triangles;   // three vertex indices per triangle
    std::vector<uint16_t> materials;  // material id per triangle

    [[nodiscard]] std::size_t triangle_count() const noexcept { return materials.size(); }
    [[nodiscard]] std::size_t vertex_count() const noexcept { return vertices.size() / 3; }
};

/// The six face-adjacent chunks, null where not loaded: -x, +x, -y, +y, -z, +z.
using Neighbours = std::array<const ChunkVoxels*, 6>;

/// Boundary-only greedy mesher (ADR 0003).
///
/// A surface is emitted on a cell's side facing a neighbour of a more open kind (MaterialKind),
/// with the cell's material; coplanar faces of one material are merged into rectangles. Faces
/// towards chunks that are not loaded are left out (the neighbour's contents are unknown; the
/// chunk is meshed again when it arrives). Partial blocks are meshed as their boxes and count
/// as open cells for their neighbours. Normals (by winding, counter-clockwise seen from the
/// front) point into the open side.
///
/// Level of detail 1 meshes 2³-block super-voxels (the majority material; ties go to the denser
/// kind) and leaves partial blocks out: the coarse ring beyond the full-detail radius.
class Mesher {
public:
    /// `kinds[material id]`; ids beyond the table count as air.
    explicit Mesher(std::vector<MaterialKind> kinds);

    [[nodiscard]] ChunkMesh mesh(const ChunkVoxels& chunk, const Neighbours& neighbours, int lod = 0) const;

    [[nodiscard]] MaterialKind kind(uint16_t material) const noexcept {
        return material < kinds_.size() ? kinds_[material] : MaterialKind::Air;
    }

private:
    std::vector<MaterialKind> kinds_;
};

}  // namespace vsa::world

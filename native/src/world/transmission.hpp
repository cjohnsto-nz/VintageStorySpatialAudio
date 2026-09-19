#pragma once

#include "world/voxel.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vsa::world {

/// What transmission needs to know about a material.
struct TransmissionMaterial {
    MaterialKind kind = MaterialKind::Air;
    /// Loss each time a path enters the material (its surfaces: a pane, a door), dB per band.
    float crossing_db[3] = {0.0f, 0.0f, 0.0f};
    /// Loss per metre inside it, dB per band.
    float bulk_db_per_metre[3] = {0.0f, 0.0f, 0.0f};
};

/// A straight path's passage through the world.
struct TransmissionTrace {
    /// Total loss per band (low, mid, high), dB.
    float loss_db[3] = {0.0f, 0.0f, 0.0f};
    /// Metres inside non-air cells and boxes.
    float solid_metres = 0.0f;
    /// Times the path entered a material.
    uint32_t crossings = 0;

    [[nodiscard]] bool blocked() const noexcept { return crossings > 0; }
    /// Amplitude gain for a band.
    [[nodiscard]] float gain(int band) const noexcept;
};

/// An immutable snapshot of the scene's voxels and material losses, for the simulation thread
/// (ADR 0004): shared chunk grids, so taking one is cheap and nothing is copied.
///
/// Positions are world block coordinates in double precision.
class VoxelView {
public:
    using Chunks = std::unordered_map<ChunkKey, std::shared_ptr<const ChunkVoxels>, ChunkKeyHash>;

    /// `origin`: the scene origin at the time of the snapshot (scene coordinates = world - origin).
    VoxelView(Chunks chunks, std::vector<TransmissionMaterial> materials, std::array<int32_t, 3> origin = {0, 0, 0});

    [[nodiscard]] const int32_t* origin() const noexcept { return origin_.data(); }

    /// The path from `from` to `to`: the loss of every material it enters (once per entry) and
    /// passes through (per metre), walking the voxel grid cell by cell and intersecting partial
    /// blocks' boxes. Chunks that are not loaded count as air.
    [[nodiscard]] TransmissionTrace trace(const double from[3], const double to[3]) const;

    /// Moves `point` towards `target` until it is out of solid cells (at most `max_cells` cells):
    /// sounds are often placed at the centre of a block (breaking, placing, a door), which must
    /// not muffle its own sound. Returns true if the point moved.
    bool escape(double point[3], const double target[3], int max_cells) const;

    /// The material of the full cell at a world block, 0 (air) if unknown or partial.
    [[nodiscard]] uint16_t material_at(int64_t x, int64_t y, int64_t z) const;

    [[nodiscard]] const std::vector<TransmissionMaterial>& materials() const noexcept { return materials_; }
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }

private:
    [[nodiscard]] const ChunkVoxels* chunk_of(int64_t x, int64_t y, int64_t z, int& cell) const;
    [[nodiscard]] MaterialKind kind(uint16_t material) const noexcept {
        return material < materials_.size() ? materials_[material].kind : MaterialKind::Air;
    }

    Chunks chunks_;
    std::vector<TransmissionMaterial> materials_;
    std::array<int32_t, 3> origin_;
};

/// Sorts a chunk's partial blocks by cell (VoxelView looks them up by binary search).
void sort_partials(ChunkVoxels& voxels);

}  // namespace vsa::world

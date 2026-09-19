#pragma once

#include "world/transmission.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace vsa::world {

/// How far sound travels through the air from one point to every cell around it (ADR 0011):
/// around pillars and corners, through doorways; not through walls. A shortest-path search over
/// the voxels (Dijkstra with chamfer steps of 3, 4 and 5 thirds of a block for face, edge and
/// corner moves; within a few percent of straight-line distance in the open), kRadius blocks each
/// way. Partial blocks (doors, fences, slabs) are passable at a penalty; solids and liquids are
/// not; foliage and unloaded chunks are open. A diagonal step may not squeeze between solids.
///
/// For the reverb: a sound in the listener's space excites it however it reaches the listener,
/// whatever blocks the straight line.
class AirField {
public:
    static constexpr int kRadius = 24;
    static constexpr double kNone = std::numeric_limits<double>::infinity();

    /// Recomputes from `from` (world block coordinates). Allocates the first time only.
    void compute(const VoxelView& view, const double from[3]);
    [[nodiscard]] bool computed() const noexcept { return computed_; }
    /// The cell it was computed from.
    [[nodiscard]] const int64_t* centre() const noexcept { return centre_; }

    /// Metres through the air from where it was computed to `point` (world coordinates): never
    /// less than the straight line; kNone if there is no path within reach.
    [[nodiscard]] double distance(const double point[3]) const;

private:
    static constexpr int kSize = 2 * kRadius + 1;
    static constexpr uint16_t kUnreached = 0xFFFF;
    // Chamfer units: a third of a block.
    static constexpr int kFace = 3;
    static constexpr int kEdge = 4;
    static constexpr int kCorner = 5;
    // Entering a partial block's cell costs this much more (4 blocks): a closed door leaks a little.
    static constexpr int kPartialPenalty = 12;

    [[nodiscard]] static std::size_t index(int x, int y, int z) noexcept {
        return (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(z)) * kSize + static_cast<std::size_t>(x);
    }

    int64_t centre_[3] = {};
    double from_[3] = {};
    bool computed_ = false;
    std::vector<uint16_t> units_;
    std::vector<VoxelView::Passage> passage_;
    std::vector<std::vector<uint32_t>> buckets_;
};

}  // namespace vsa::world

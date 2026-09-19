#include "world/transmission.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vsa::world {
namespace {

constexpr int kMaxSteps = 2048;  // cells along one path (about 1 km diagonally)

/// Length of the segment p0 + d * t, t in [t0, t1], inside the box [lo, hi].
double segment_in_box(const double p0[3], const double d[3], double t0, double t1, const double lo[3], const double hi[3]) {
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12) {
            if (p0[a] < lo[a] || p0[a] > hi[a]) {
                return 0.0;
            }
            continue;
        }
        double ta = (lo[a] - p0[a]) / d[a];
        double tb = (hi[a] - p0[a]) / d[a];
        if (ta > tb) {
            std::swap(ta, tb);
        }
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 >= t1) {
            return 0.0;
        }
    }
    return t1 - t0;  // in units of |d| (d is the whole path, so callers scale by its length)
}

}  // namespace

float TransmissionTrace::gain(int band) const noexcept {
    return std::pow(10.0f, -loss_db[band] / 20.0f);
}

void sort_partials(ChunkVoxels& voxels) {
    std::stable_sort(voxels.partials.begin(), voxels.partials.end(),
                     [](const PartialBlock& a, const PartialBlock& b) { return a.cell < b.cell; });
}

VoxelView::VoxelView(Chunks chunks, std::vector<TransmissionMaterial> materials, std::array<int32_t, 3> origin)
    : chunks_(std::move(chunks)), materials_(std::move(materials)), origin_(origin) {}

const ChunkVoxels* VoxelView::chunk_of(int64_t x, int64_t y, int64_t z, int& cell) const {
    const ChunkKey key{static_cast<int32_t>(x >> 5), static_cast<int32_t>(y >> 5), static_cast<int32_t>(z >> 5)};
    const auto it = chunks_.find(key);
    if (it == chunks_.end()) {
        return nullptr;
    }
    cell = cell_index(static_cast<int>(x & 31), static_cast<int>(y & 31), static_cast<int>(z & 31));
    return it->second.get();
}

uint16_t VoxelView::material_at(int64_t x, int64_t y, int64_t z) const {
    int cell = 0;
    const ChunkVoxels* chunk = chunk_of(x, y, z, cell);
    return chunk != nullptr ? chunk->materials[static_cast<std::size_t>(cell)] : kAir;
}

TransmissionTrace VoxelView::trace(const double from[3], const double to[3]) const {
    TransmissionTrace result;
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < 1e-9) {
        return result;
    }

    // Amanatides–Woo over unit cells, t in [0, 1] along the whole path.
    int64_t cell[3];
    int64_t step[3];
    double t_max[3];
    double t_delta[3];
    for (int a = 0; a < 3; ++a) {
        cell[a] = static_cast<int64_t>(std::floor(from[a]));
        if (d[a] > 0.0) {
            step[a] = 1;
            t_max[a] = (static_cast<double>(cell[a] + 1) - from[a]) / d[a];
            t_delta[a] = 1.0 / d[a];
        } else if (d[a] < 0.0) {
            step[a] = -1;
            t_max[a] = (from[a] - static_cast<double>(cell[a])) / -d[a];
            t_delta[a] = -1.0 / d[a];
        } else {
            step[a] = 0;
            t_max[a] = std::numeric_limits<double>::infinity();
            t_delta[a] = std::numeric_limits<double>::infinity();
        }
    }

    double loss[3] = {0.0, 0.0, 0.0};
    double solid = 0.0;
    int current = -1;  // material of the run the path is in, -1 in air
    double t = 0.0;
    for (int steps = 0; steps < kMaxSteps && t < 1.0; ++steps) {
        const int axis = t_max[0] < t_max[1] ? (t_max[0] < t_max[2] ? 0 : 2) : (t_max[1] < t_max[2] ? 1 : 2);
        const double t_exit = std::min(t_max[axis], 1.0);
        const double metres = (t_exit - t) * length;

        int index = 0;
        const ChunkVoxels* chunk = chunk_of(cell[0], cell[1], cell[2], index);
        const uint16_t material = chunk != nullptr ? chunk->materials[static_cast<std::size_t>(index)] : kAir;
        if (kind(material) != MaterialKind::Air) {
            const TransmissionMaterial& m = materials_[material];
            if (current != material) {
                ++result.crossings;
                for (int b = 0; b < 3; ++b) {
                    loss[b] += static_cast<double>(m.crossing_db[b]);
                }
                current = material;
            }
            for (int b = 0; b < 3; ++b) {
                loss[b] += static_cast<double>(m.bulk_db_per_metre[b]) * metres;
            }
            solid += metres;
        } else {
            current = -1;
            if (chunk != nullptr && !chunk->partials.empty()) {
                // Partial blocks in this cell: each box the path passes through is a crossing.
                const auto cell16 = static_cast<uint16_t>(index);
                auto it = std::lower_bound(chunk->partials.begin(), chunk->partials.end(), cell16,
                                           [](const PartialBlock& p, uint16_t c) { return p.cell < c; });
                for (; it != chunk->partials.end() && it->cell == cell16; ++it) {
                    if (kind(it->material) == MaterialKind::Air) {
                        continue;
                    }
                    const TransmissionMaterial& m = materials_[it->material];
                    for (const Box& box : it->boxes) {
                        double lo[3];
                        double hi[3];
                        for (int a = 0; a < 3; ++a) {
                            lo[a] = static_cast<double>(cell[a]) + static_cast<double>(box.min[a]);
                            hi[a] = static_cast<double>(cell[a]) + static_cast<double>(box.max[a]);
                        }
                        const double inside = segment_in_box(from, d, t, t_exit, lo, hi) * length;
                        if (inside > 1e-4) {
                            ++result.crossings;
                            for (int b = 0; b < 3; ++b) {
                                loss[b] += static_cast<double>(m.crossing_db[b]) +
                                           static_cast<double>(m.bulk_db_per_metre[b]) * inside;
                            }
                            solid += inside;
                        }
                    }
                }
            }
        }

        t = t_exit;
        cell[axis] += step[axis];
        t_max[axis] += t_delta[axis];
    }

    for (int b = 0; b < 3; ++b) {
        result.loss_db[b] = static_cast<float>(loss[b]);
    }
    result.solid_metres = static_cast<float>(solid);
    return result;
}

bool VoxelView::escape(double point[3], const double target[3], int max_cells) const {
    bool moved = false;
    for (int i = 0; i < max_cells; ++i) {
        const auto cx = static_cast<int64_t>(std::floor(point[0]));
        const auto cy = static_cast<int64_t>(std::floor(point[1]));
        const auto cz = static_cast<int64_t>(std::floor(point[2]));
        if (kind(material_at(cx, cy, cz)) != MaterialKind::Solid) {
            return moved;
        }
        // Step to where the line towards the target leaves this cell, just past the face.
        const double d[3] = {target[0] - point[0], target[1] - point[1], target[2] - point[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-6) {
            return moved;
        }
        const int64_t c[3] = {cx, cy, cz};
        double t_exit = std::numeric_limits<double>::infinity();
        for (int a = 0; a < 3; ++a) {
            if (d[a] > 0.0) {
                t_exit = std::min(t_exit, (static_cast<double>(c[a] + 1) - point[a]) / d[a]);
            } else if (d[a] < 0.0) {
                t_exit = std::min(t_exit, (static_cast<double>(c[a]) - point[a]) / d[a]);
            }
        }
        if (t_exit >= 1.0) {
            return moved;  // the target is inside this very cell
        }
        const double nudge = 1e-3 / len;
        for (int a = 0; a < 3; ++a) {
            point[a] += d[a] * (t_exit + nudge);
        }
        moved = true;
    }
    return moved;
}

}  // namespace vsa::world

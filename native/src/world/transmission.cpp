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
    double run = 0.0;  // metres in the current run
    // A run of one material ends: its crossing loss, in proportion to how far it went in.
    const auto end_run = [&] {
        if (current >= 0 && run >= TransmissionTrace::kMinRun) {
            const double weight = std::min(1.0, run / TransmissionTrace::kFullCrossingRun);
            const TransmissionMaterial& m = materials_[static_cast<std::size_t>(current)];
            for (int b = 0; b < 3; ++b) {
                loss[b] += weight * static_cast<double>(m.crossing_db[b]);
            }
            ++result.crossings;
        }
        current = -1;
        run = 0.0;
    };
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
                end_run();
                current = material;
            }
            run += metres;
            for (int b = 0; b < 3; ++b) {
                loss[b] += static_cast<double>(m.bulk_db_per_metre[b]) * metres;
            }
            solid += metres;
        } else {
            end_run();
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
                        if (inside > TransmissionTrace::kMinRun * 0.5) {  // boxes are thin: a door counts in full
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
    end_run();

    for (int b = 0; b < 3; ++b) {
        result.loss_db[b] = static_cast<float>(loss[b]);
    }
    result.solid_metres = static_cast<float>(solid);
    return result;
}

bool VoxelView::first_hit(const double origin[3], const double direction[3], double max_distance,
                          VoxelHit& hit) const {
    int64_t cell[3];
    int64_t step[3];
    double t_max[3];
    double t_delta[3];
    for (int a = 0; a < 3; ++a) {
        cell[a] = static_cast<int64_t>(std::floor(origin[a]));
        const double d = direction[a];
        if (d > 0.0) {
            step[a] = 1;
            t_max[a] = (static_cast<double>(cell[a] + 1) - origin[a]) / d;
            t_delta[a] = 1.0 / d;
        } else if (d < 0.0) {
            step[a] = -1;
            t_max[a] = (origin[a] - static_cast<double>(cell[a])) / -d;
            t_delta[a] = -1.0 / d;
        } else {
            step[a] = 0;
            t_max[a] = std::numeric_limits<double>::infinity();
            t_delta[a] = std::numeric_limits<double>::infinity();
        }
    }
    constexpr double kEpsilon = 1e-6;
    double t = 0.0;
    int entered = -1;  // the axis crossed into the current cell; -1 for the starting cell
    for (int steps = 0; steps < kMaxSteps && t <= max_distance; ++steps) {
        const int axis = t_max[0] < t_max[1] ? (t_max[0] < t_max[2] ? 0 : 2) : (t_max[1] < t_max[2] ? 1 : 2);
        const double t_exit = t_max[axis];
        int index = 0;
        const ChunkVoxels* chunk = chunk_of(cell[0], cell[1], cell[2], index);
        const uint16_t material = chunk != nullptr ? chunk->materials[static_cast<std::size_t>(index)] : kAir;
        if (entered >= 0 && kind(material) != MaterialKind::Air) {
            hit.distance = t;
            for (int a = 0; a < 3; ++a) {
                hit.point[a] = origin[a] + direction[a] * t;
                hit.normal[a] = a == entered ? static_cast<double>(-step[a]) : 0.0;
            }
            hit.material = material;
            hit.partial = false;
            return true;
        }
        if (chunk != nullptr && !chunk->partials.empty()) {
            const auto cell16 = static_cast<uint16_t>(index);
            auto it = std::lower_bound(chunk->partials.begin(), chunk->partials.end(), cell16,
                                       [](const PartialBlock& p, uint16_t c) { return p.cell < c; });
            double best = std::min(t_exit, max_distance);
            bool found = false;
            for (; it != chunk->partials.end() && it->cell == cell16; ++it) {
                if (kind(it->material) == MaterialKind::Air) {
                    continue;
                }
                for (const Box& box : it->boxes) {
                    // Slab test: where the ray enters the box, and through which face.
                    double near = -std::numeric_limits<double>::infinity();
                    double far = std::numeric_limits<double>::infinity();
                    int face = -1;
                    bool miss = false;
                    for (int a = 0; a < 3 && !miss; ++a) {
                        const double lo = static_cast<double>(cell[a]) + static_cast<double>(box.min[a]);
                        const double hi = static_cast<double>(cell[a]) + static_cast<double>(box.max[a]);
                        if (std::abs(direction[a]) < 1e-12) {
                            miss = origin[a] < lo || origin[a] > hi;
                            continue;
                        }
                        double ta = (lo - origin[a]) / direction[a];
                        double tb = (hi - origin[a]) / direction[a];
                        if (ta > tb) {
                            std::swap(ta, tb);
                        }
                        if (ta > near) {
                            near = ta;
                            face = a;
                        }
                        far = std::min(far, tb);
                    }
                    if (miss || face < 0 || near > far || near <= kEpsilon || near >= best) {
                        continue;  // missed, or starts inside the box
                    }
                    best = near;
                    found = true;
                    hit.distance = near;
                    for (int a = 0; a < 3; ++a) {
                        hit.point[a] = origin[a] + direction[a] * near;
                        hit.normal[a] = a == face ? (direction[a] > 0.0 ? -1.0 : 1.0) : 0.0;
                    }
                    hit.material = it->material;
                    hit.partial = true;
                }
            }
            if (found) {
                return true;
            }
        }
        t = t_exit;
        cell[axis] += step[axis];
        t_max[axis] += t_delta[axis];
        entered = axis;
    }
    return false;
}

void VoxelView::clearance(double point[3], double radius) const {
    const int64_t c[3] = {static_cast<int64_t>(std::floor(point[0])), static_cast<int64_t>(std::floor(point[1])),
                          static_cast<int64_t>(std::floor(point[2]))};
    if (kind(material_at(c[0], c[1], c[2])) == MaterialKind::Solid) {
        return;  // inside rock: escape() is for that
    }
    const double r = std::clamp(radius, 0.0, 0.49);
    for (int a = 0; a < 3; ++a) {
        int64_t below[3] = {c[0], c[1], c[2]};
        int64_t above[3] = {c[0], c[1], c[2]};
        below[a] -= 1;
        above[a] += 1;
        const double frac = point[a] - static_cast<double>(c[a]);
        if (frac < r && kind(material_at(below[0], below[1], below[2])) == MaterialKind::Solid) {
            point[a] = static_cast<double>(c[a]) + r;
        } else if (frac > 1.0 - r && kind(material_at(above[0], above[1], above[2])) == MaterialKind::Solid) {
            point[a] = static_cast<double>(c[a]) + 1.0 - r;
        }
    }
}

VoxelView::Passage VoxelView::passage(int64_t x, int64_t y, int64_t z) const {
    int index = 0;
    const ChunkVoxels* chunk = chunk_of(x, y, z, index);
    if (chunk == nullptr) {
        return Passage::Open;
    }
    switch (kind(chunk->materials[static_cast<std::size_t>(index)])) {
        case MaterialKind::Solid:
        case MaterialKind::Liquid: return Passage::Closed;
        case MaterialKind::Porous: return Passage::Open;
        case MaterialKind::Air: break;
    }
    return has_partial(x, y, z) ? Passage::Partial : Passage::Open;
}

bool VoxelView::has_partial(int64_t x, int64_t y, int64_t z) const {
    int index = 0;
    const ChunkVoxels* chunk = chunk_of(x, y, z, index);
    if (chunk == nullptr || chunk->partials.empty()) {
        return false;
    }
    const auto cell16 = static_cast<uint16_t>(index);
    auto it = std::lower_bound(chunk->partials.begin(), chunk->partials.end(), cell16,
                               [](const PartialBlock& p, uint16_t c) { return p.cell < c; });
    for (; it != chunk->partials.end() && it->cell == cell16; ++it) {
        if (kind(it->material) != MaterialKind::Air && !it->boxes.empty()) {
            return true;
        }
    }
    return false;
}

bool VoxelView::escape(double point[3], const double target[3], int max_cells, double beyond) const {
    bool moved = false;
    for (int i = 0; i < max_cells; ++i) {
        const auto cx = static_cast<int64_t>(std::floor(point[0]));
        const auto cy = static_cast<int64_t>(std::floor(point[1]));
        const auto cz = static_cast<int64_t>(std::floor(point[2]));
        if (kind(material_at(cx, cy, cz)) != MaterialKind::Solid && !has_partial(cx, cy, cz)) {
            break;
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
    if (moved && beyond > 1e-3) {
        // Further off the block's face, unless that runs into another solid cell or the target.
        const double d[3] = {target[0] - point[0], target[1] - point[1], target[2] - point[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double step = std::min(beyond, len * 0.5);
        if (len > 1e-6) {
            double next[3];
            for (int a = 0; a < 3; ++a) {
                next[a] = point[a] + d[a] / len * step;
            }
            if (kind(material_at(static_cast<int64_t>(std::floor(next[0])), static_cast<int64_t>(std::floor(next[1])),
                                 static_cast<int64_t>(std::floor(next[2])))) != MaterialKind::Solid) {
                std::copy_n(next, 3, point);
            }
        }
    }
    return moved;
}

}  // namespace vsa::world

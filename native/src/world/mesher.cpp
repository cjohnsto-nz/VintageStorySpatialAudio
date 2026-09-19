#include "world/mesher.hpp"

#include <algorithm>
#include <utility>

namespace vsa::world {
namespace {

constexpr int kNone = -1;

/// Emits a quad on the plane `axis` = `plane`, spanning [u0, u1] x [v0, v1] on the other two axes
/// (u = axis + 1, v = axis + 2, cyclically), facing +axis when `positive`, else -axis.
void emit_quad(ChunkMesh& mesh, int axis, bool positive, float plane, float u0, float u1, float v0, float v1,
               uint16_t material) {
    const int u = (axis + 1) % 3;
    const int v = (axis + 2) % 3;
    const auto base = static_cast<int32_t>(mesh.vertex_count());
    const float corners[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (const auto& corner : corners) {
        float p[3];
        p[axis] = plane;
        p[u] = corner[0];
        p[v] = corner[1];
        mesh.vertices.insert(mesh.vertices.end(), {p[0], p[1], p[2]});
    }
    // e_u x e_v = e_axis: this winding faces +axis; reversed faces -axis.
    if (positive) {
        mesh.triangles.insert(mesh.triangles.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    } else {
        mesh.triangles.insert(mesh.triangles.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    mesh.materials.insert(mesh.materials.end(), {material, material});
}

}  // namespace

Mesher::Mesher(std::vector<MaterialKind> kinds) : kinds_(std::move(kinds)) {}

ChunkMesh Mesher::mesh(const ChunkVoxels& chunk, const Neighbours& neighbours, int lod) const {
    const int step = lod > 0 ? 2 : 1;
    const int n = kChunkSize / step;

    // The material of a (possibly coarse) cell of `voxels`, coordinates in cells of this size.
    const auto sample = [&](const ChunkVoxels& voxels, int x, int y, int z) -> uint16_t {
        if (step == 1) {
            return voxels.materials[static_cast<std::size_t>(cell_index(x, y, z))];
        }
        std::array<uint16_t, 8> found{};
        std::array<int, 8> counts{};
        int distinct = 0;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dz = 0; dz < 2; ++dz) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint16_t m =
                        voxels.materials[static_cast<std::size_t>(cell_index(x * 2 + dx, y * 2 + dy, z * 2 + dz))];
                    int i = 0;
                    while (i < distinct && found[static_cast<std::size_t>(i)] != m) {
                        ++i;
                    }
                    if (i == distinct) {
                        found[static_cast<std::size_t>(distinct++)] = m;
                    }
                    ++counts[static_cast<std::size_t>(i)];
                }
            }
        }
        std::size_t best = 0;
        for (std::size_t i = 1; i < static_cast<std::size_t>(distinct); ++i) {
            if (counts[i] > counts[best] || (counts[i] == counts[best] && kind(found[i]) > kind(found[best]))) {
                best = i;
            }
        }
        return found[best];
    };

    // The chunk's cells plus a one-cell border from the neighbours (kNone where unloaded).
    const int padded = n + 2;
    std::vector<int> grid(static_cast<std::size_t>(padded) * static_cast<std::size_t>(padded) *
                              static_cast<std::size_t>(padded),
                          kNone);
    const auto at = [padded](int x, int y, int z) {
        return static_cast<std::size_t>(((y + 1) * padded + (z + 1)) * padded + (x + 1));
    };
    for (int y = -1; y <= n; ++y) {
        for (int z = -1; z <= n; ++z) {
            for (int x = -1; x <= n; ++x) {
                const int c[3] = {x, y, z};
                int outside = 0;
                int axis_out = -1;
                for (int a = 0; a < 3; ++a) {
                    if (c[a] < 0 || c[a] >= n) {
                        ++outside;
                        axis_out = a;
                    }
                }
                if (outside == 0) {
                    grid[at(x, y, z)] = sample(chunk, x, y, z);
                } else if (outside == 1) {  // face neighbours only; edges and corners are never read
                    const ChunkVoxels* other =
                        neighbours[static_cast<std::size_t>(axis_out * 2 + (c[axis_out] < 0 ? 0 : 1))];
                    if (other != nullptr) {
                        int wrapped[3] = {x, y, z};
                        wrapped[axis_out] = c[axis_out] < 0 ? n - 1 : 0;
                        grid[at(x, y, z)] = sample(*other, wrapped[0], wrapped[1], wrapped[2]);
                    }
                }
            }
        }
    }
    ChunkMesh mesh;
    std::vector<int> mask(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    const auto fstep = static_cast<float>(step);
    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        for (const int sign : {-1, 1}) {
            for (int slice = 0; slice < n; ++slice) {
                // Faces on this slice's `sign` side: the cell's material, or kNone.
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        int cell[3];
                        cell[axis] = slice;
                        cell[u] = i;
                        cell[v] = j;
                        const auto own = static_cast<uint16_t>(grid[at(cell[0], cell[1], cell[2])]);
                        cell[axis] += sign;
                        const int other = grid[at(cell[0], cell[1], cell[2])];
                        const bool face = other != kNone && kind(own) > kind(static_cast<uint16_t>(other));
                        mask[static_cast<std::size_t>(j * n + i)] = face ? own : kNone;
                    }
                }
                // Greedy: widest run along u, then as many identical rows along v as possible.
                const float plane = static_cast<float>(sign > 0 ? slice + 1 : slice) * fstep;
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n;) {
                        const int m = mask[static_cast<std::size_t>(j * n + i)];
                        if (m == kNone) {
                            ++i;
                            continue;
                        }
                        int width = 1;
                        while (i + width < n && mask[static_cast<std::size_t>(j * n + i + width)] == m) {
                            ++width;
                        }
                        int height = 1;
                        for (; j + height < n; ++height) {
                            bool row = true;
                            for (int k = 0; k < width && row; ++k) {
                                row = mask[static_cast<std::size_t>((j + height) * n + i + k)] == m;
                            }
                            if (!row) {
                                break;
                            }
                        }
                        for (int dj = 0; dj < height; ++dj) {
                            std::fill_n(mask.begin() + (j + dj) * n + i, width, kNone);
                        }
                        emit_quad(mesh, axis, sign > 0, plane, static_cast<float>(i) * fstep,
                                  static_cast<float>(i + width) * fstep, static_cast<float>(j) * fstep,
                                  static_cast<float>(j + height) * fstep, static_cast<uint16_t>(m));
                        i += width;
                    }
                }
            }
        }
    }

    mesh.first_partial_triangle = mesh.triangle_count();
    if (lod == 0) {
        for (const PartialBlock& block : chunk.partials) {
            if (kind(block.material) == MaterialKind::Air) {
                continue;
            }
            const int cell = block.cell;
            const float origin[3] = {static_cast<float>(cell % kChunkSize),
                                     static_cast<float>(cell / (kChunkSize * kChunkSize)),
                                     static_cast<float>((cell / kChunkSize) % kChunkSize)};
            for (const Box& box : block.boxes) {
                float lo[3];
                float hi[3];
                for (int a = 0; a < 3; ++a) {
                    lo[a] = origin[a] + std::clamp(box.min[a], 0.0f, 1.0f);
                    hi[a] = origin[a] + std::clamp(box.max[a], 0.0f, 1.0f);
                }
                if (hi[0] <= lo[0] || hi[1] <= lo[1] || hi[2] <= lo[2]) {
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    const int u = (axis + 1) % 3;
                    const int v = (axis + 2) % 3;
                    emit_quad(mesh, axis, false, lo[axis], lo[u], hi[u], lo[v], hi[v], block.material);
                    emit_quad(mesh, axis, true, hi[axis], lo[u], hi[u], lo[v], hi[v], block.material);
                }
            }
        }
    }
    return mesh;
}

}  // namespace vsa::world

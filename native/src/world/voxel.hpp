#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vsa::world {

/// Blocks per chunk edge (Vintage Story chunks are 32³).
inline constexpr int kChunkSize = 32;
inline constexpr int kChunkCells = kChunkSize * kChunkSize * kChunkSize;

/// Cell index within a chunk: x fastest, then z, then y (the ABI's order).
[[nodiscard]] constexpr int cell_index(int x, int y, int z) noexcept { return (y * kChunkSize + z) * kChunkSize + x; }

/// Material id 0 is air everywhere.
inline constexpr uint16_t kAir = 0;

/// How open a material is to sound, from the meshing's point of view. A surface is emitted where
/// a denser cell meets a more open one (solid rock next to air, a water surface under air); cells
/// of equal kind share no surface (buried stone against soil), since transmission through them is
/// computed from the voxel grid (ADR 0004), not from surfaces.
enum class MaterialKind : uint8_t { Air = 0, Liquid = 1, Porous = 2, Solid = 3 };

struct ChunkKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    friend bool operator==(const ChunkKey&, const ChunkKey&) = default;
};

struct ChunkKeyHash {
    std::size_t operator()(const ChunkKey& k) const noexcept {
        const auto h = static_cast<uint64_t>(static_cast<uint32_t>(k.x)) * 0x9E3779B97F4A7C15ull ^
                       static_cast<uint64_t>(static_cast<uint32_t>(k.y)) * 0xC2B2AE3D27D4EB4Full ^
                       static_cast<uint64_t>(static_cast<uint32_t>(k.z)) * 0x165667B19E3779F9ull;
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

/// A box inside a block, in block units (0..1 per axis): part of a partial block's shape.
struct Box {
    float min[3];
    float max[3];
};

/// A partial block (slab, stairs, fence, open door, …): its collision boxes, meshed as box
/// surfaces. Its cell counts as open (air) for its neighbours' surfaces.
struct PartialBlock {
    uint16_t cell = 0;  // cell_index
    uint16_t material = kAir;
    std::vector<Box> boxes;
};

/// One chunk's acoustic contents: a material id per cell (partial blocks' cells hold kAir) and
/// the partial blocks.
struct ChunkVoxels {
    std::vector<uint16_t> materials = std::vector<uint16_t>(kChunkCells, kAir);
    std::vector<PartialBlock> partials;
};

}  // namespace vsa::world

#pragma once

#include "steam/ipl_handle.hpp"
#include "world/mesher.hpp"
#include "world/transmission.hpp"
#include "world/voxel.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vsa::steam {
class SteamContext;
}

namespace vsa::world {

/// An acoustic material: Steam Audio surface properties for the mesh, plus the per-metre
/// attenuation the voxel transmission uses (ADR 0004).
struct AcousticMaterial {
    std::string name;
    MaterialKind kind = MaterialKind::Solid;
    float absorption[3] = {0.1f, 0.1f, 0.1f};
    float scattering = 0.05f;
    float transmission[3] = {0.0f, 0.0f, 0.0f};
    float attenuation_db_per_metre[3] = {0.0f, 0.0f, 0.0f};
};

/// A ray's first hit on the scene's meshes (see WorldScene::raycast).
struct RayHit {
    float distance = 0.0f;
    float point[3] = {};   // scene coordinates (relative to the origin)
    float normal[3] = {};  // the surface's front (the open side)
    ChunkKey chunk;
    uint32_t triangle = 0;
    uint16_t material = 0;
    int lod = 0;
    bool from_partial = false;   // a partial block's box, else a whole cell's face
    int32_t cell[3] = {};        // world block that produced the surface (just behind it)
};

struct SceneStats {
    uint32_t chunks = 0;          // chunks the scene holds (voxels)
    uint32_t meshed_chunks = 0;   // with geometry in the Steam Audio scene
    uint32_t pending_chunks = 0;  // waiting to be (re)meshed or removed
    uint64_t triangles = 0;
    uint64_t vertices = 0;
    uint64_t memory_bytes = 0;    // voxels + meshes (not Steam Audio's own copies)
    uint64_t chunks_built = 0;    // total meshing jobs done
    double last_build_ms = 0.0;   // mesh + Steam Audio objects for one chunk
    double max_build_ms = 0.0;
    double last_commit_ms = 0.0;  // top-level scene commit
    int32_t origin[3] = {0, 0, 0};
};

/// The world as a Steam Audio scene (ADR 0003): a top-level scene holding one instanced mesh per
/// chunk, whose sub-scene holds the chunk's static mesh from the boundary mesher.
///
/// Chunk edits change the top-level scene in place (remove the chunk's instance, add its new one),
/// with one rule: nothing removed is released while that scene lives. Steam Audio 4.8.1's Embree
/// scene gives a released mesh's geometry id to the next mesh without detaching the old geometry,
/// so the new one fails to attach and is silently missing (tests/core/test_embree_scene_edits.cpp;
/// in game, occlusion vanished once doors had been opened). Removed instances, and the chunk
/// sub-scenes they reference, wait in the scene's graveyard; when it grows past a quarter of the
/// live instances (at least 64), a fresh top-level scene is built without any lock held, the
/// attached simulators switch to it, and the old one is released whole.
///
/// Chunks arrive as voxel snapshots and are meshed on the scene's own worker thread; a chunk's
/// arrival or removal also re-meshes its loaded neighbours (their border faces change). Vertices
/// are chunk-local; each instance is placed at its chunk's position relative to the origin, a
/// block position near the listener that keeps coordinates small (Vintage Story's are ~500 000,
/// where a float resolves 6 cm), and which set_origin moves by updating the transforms.
///
/// Every Steam Audio object is removed and committed before it is released (the Phase 0
/// LeakSanitizer finding). Thread-safe; nothing here is for the render thread.
class WorldScene {
public:
    explicit WorldScene(const steam::SteamContext& steam);
    ~WorldScene();

    WorldScene(const WorldScene&) = delete;
    WorldScene& operator=(const WorldScene&) = delete;

    /// Replaces the material table (id = index; id 0 is air) and re-meshes every chunk.
    void set_materials(std::vector<AcousticMaterial> materials);
    /// Block position of the scene origin.
    void set_origin(int32_t x, int32_t y, int32_t z);
    /// Adds or replaces a chunk (queued for meshing). `lod` 0 = full detail, 1 = 2³ super-voxels.
    void set_chunk(ChunkKey key, std::shared_ptr<const ChunkVoxels> voxels, int lod);
    void remove_chunk(ChunkKey key);
    void clear();

    /// Waits until nothing is pending and the top-level scene is committed. False on timeout.
    bool wait_idle(std::chrono::milliseconds timeout);

    [[nodiscard]] SceneStats stats() const;
    /// The mesh exactly as submitted to Steam Audio (chunk-local), and its level of detail.
    /// False if the chunk has no mesh (unknown, pending, or all air).
    bool chunk_mesh(ChunkKey key, std::shared_ptr<const ChunkMesh>& mesh, int& lod) const;
    bool chunk_mesh(ChunkKey key, std::shared_ptr<const ChunkMesh>& mesh, int& lod, uint32_t& version) const;
    [[nodiscard]] std::vector<ChunkKey> chunk_keys() const;
    /// Writes every chunk's mesh as an OBJ (+ .mtl) in world block coordinates, one group per
    /// chunk and one material per acoustic material. Throws vsa::Error on I/O failure.
    void save_obj(const std::string& path) const;
    [[nodiscard]] std::vector<AcousticMaterial> materials() const;
    /// The first triangle of the meshed scene a ray hits (scene coordinates), for debugging:
    /// exactly the geometry Steam Audio has. Either side of a triangle counts.
    [[nodiscard]] bool raycast(const float origin[3], const float direction[3], float max_distance, RayHit& hit) const;
    [[nodiscard]] std::size_t material_count() const;

    /// The voxels and material losses as they are now, for transmission (cheap: shares the chunk
    /// grids; rebuilt only after a change).
    [[nodiscard]] std::shared_ptr<const VoxelView> voxel_view() const;

    /// Attaches a simulator: it is given the current top-level scene now and every new one as it
    /// is built (under scene_lock(), which the simulator must hold while it runs). Detach before
    /// destroying the simulator or this scene.
    void attach(IPLSimulator simulator);
    void detach(IPLSimulator simulator);
    [[nodiscard]] std::mutex& scene_lock() noexcept { return scene_mutex_; }
    /// Top-level scenes built so far.
    [[nodiscard]] uint64_t commit_count() const noexcept { return commits_.load(std::memory_order_acquire); }

private:
    struct Built;
    struct Top {
        steam::Scene scene;
        std::unordered_map<ChunkKey, steam::InstancedMesh, ChunkKeyHash> live;
        std::vector<steam::InstancedMesh> graveyard;  // removed from `scene`, not released (see above)
        std::vector<std::unique_ptr<Built>> buried;   // chunk builds the graveyard still instances
    };
    struct Chunk {
        std::shared_ptr<const ChunkVoxels> voxels;  // null: removal pending
        int lod = 0;
        uint64_t version = 0;
        std::unique_ptr<Built> built;
    };

    void worker_main();
    void mark_dirty_locked(ChunkKey key);
    void mark_neighbours_dirty_locked(ChunkKey key);
    [[nodiscard]] IPLMatrix4x4 transform_locked(ChunkKey key) const;
    /// What a top-level scene instances: each built chunk's sub-scene and its placement.
    using Instances = std::vector<std::tuple<ChunkKey, IPLScene, IPLMatrix4x4>>;
    [[nodiscard]] Instances instances_locked() const;  // requires mutex_
    /// A new top-level scene (no lock needed: sub-scenes are immutable and only the worker
    /// releases them).
    [[nodiscard]] std::unique_ptr<Top> build_top(const Instances& instances) const;
    /// Removes a top-level scene's instances, commits, and releases it with its graveyard.
    static void retire(std::unique_ptr<Top>& top) noexcept;
    /// Replaces the top-level scene with a fresh one (drops the graveyard). Worker only.
    void compact();

    const steam::SteamContext& steam_;
    mutable std::mutex scene_mutex_;  // the current top-level scene and the attached simulators
    std::unique_ptr<Top> top_;
    std::vector<IPLSimulator> simulators_;
    std::atomic<uint64_t> commits_{0};

    mutable std::mutex mutex_;  // everything below
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks_;
    std::unordered_set<ChunkKey, ChunkKeyHash> dirty_;
    std::vector<AcousticMaterial> materials_;
    std::vector<IPLMaterial> ipl_materials_;
    std::shared_ptr<const Mesher> mesher_;
    int32_t origin_[3] = {0, 0, 0};
    bool origin_dirty_ = false;
    uint64_t revision_ = 1;  // bumped by every change voxel_view reflects
    mutable uint64_t view_revision_ = 0;
    mutable std::shared_ptr<const VoxelView> view_;
    bool busy_ = false;
    bool stop_ = false;
    SceneStats stats_;
    std::thread worker_;
};

}  // namespace vsa::world

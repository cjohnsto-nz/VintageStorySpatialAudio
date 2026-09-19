#include "world/world_scene.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "steam/steam_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <fstream>
#include <iomanip>

namespace vsa::world {
namespace {

void check(IPLerror error, const char* what) {
    if (error != IPL_STATUS_SUCCESS) {
        throw Error(VSA_ERROR_STEAM_AUDIO, std::string(what) + " failed: " + steam::error_name(error));
    }
}

constexpr std::size_t kBatch = 16;  // chunks per top-level commit

constexpr std::array<ChunkKey, 6> kFaceOffsets = {{{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};

ChunkKey offset(ChunkKey key, const ChunkKey& d) { return {key.x + d.x, key.y + d.y, key.z + d.z}; }

/// Whether the cell layer a chunk shares with its neighbour on face `f` differs between two
/// snapshots (only then do the neighbour's border faces change).
bool boundary_differs(const ChunkVoxels& a, const ChunkVoxels& b, std::size_t f) {
    const int axis = static_cast<int>(f / 2);
    const int layer = f % 2 == 0 ? 0 : kChunkSize - 1;
    for (int j = 0; j < kChunkSize; ++j) {
        for (int i = 0; i < kChunkSize; ++i) {
            int c[3];
            c[axis] = layer;
            c[(axis + 1) % 3] = i;
            c[(axis + 2) % 3] = j;
            const auto index = static_cast<std::size_t>(cell_index(c[0], c[1], c[2]));
            if (a.materials[index] != b.materials[index]) {
                return true;
            }
        }
    }
    return false;
}

double since_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

/// A chunk's Steam Audio objects. Its static mesh is in its own sub-scene; the instance is in the
/// top-level scene while `in_top`, and must be removed and the top committed before destruction.
struct WorldScene::Built {
    std::shared_ptr<const ChunkMesh> mesh;
    int lod = 0;
    uint32_t version = 0;  // the build number, for caching debug views
    steam::Scene sub;
    steam::StaticMesh static_mesh;
    steam::InstancedMesh instance;
    bool in_top = false;

    ~Built() {
        if (static_mesh) {
            iplStaticMeshRemove(static_mesh.get(), sub.get());
            iplSceneCommit(sub.get());
        }
        // Release order: instance (holds the sub-scene), static mesh, sub-scene.
        instance.reset();
        static_mesh.reset();
        sub.reset();
    }
};

WorldScene::WorldScene(const steam::SteamContext& steam) : steam_(steam) {
    IPLSceneSettings settings = steam_.scene_settings();
    check(iplSceneCreate(steam_.context(), &settings, top_.out()), "iplSceneCreate");
    iplSceneCommit(top_.get());
    mesher_ = std::make_shared<Mesher>(std::vector<MaterialKind>{});
    worker_ = std::thread([this] { worker_main(); });
}

WorldScene::~WorldScene() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
    // Out of the top scene first (one commit), then release everything.
    std::lock_guard scene(scene_mutex_);
    for (auto& [key, chunk] : chunks_) {
        if (chunk.built && chunk.built->in_top) {
            iplInstancedMeshRemove(chunk.built->instance.get(), top_.get());
            chunk.built->in_top = false;
        }
    }
    iplSceneCommit(top_.get());
    chunks_.clear();
}

void WorldScene::set_materials(std::vector<AcousticMaterial> materials) {
    std::vector<MaterialKind> kinds;
    std::vector<IPLMaterial> ipl;
    for (const AcousticMaterial& m : materials) {
        kinds.push_back(m.kind);
        IPLMaterial material{};
        for (int b = 0; b < 3; ++b) {
            material.absorption[b] = std::clamp(m.absorption[b], 0.0f, 1.0f);
            material.transmission[b] = std::clamp(m.transmission[b], 0.0f, 1.0f);
        }
        material.scattering = std::clamp(m.scattering, 0.0f, 1.0f);
        ipl.push_back(material);
    }
    std::lock_guard lock(mutex_);
    materials_ = std::move(materials);
    ipl_materials_ = std::move(ipl);
    mesher_ = std::make_shared<Mesher>(std::move(kinds));
    for (const auto& [key, chunk] : chunks_) {
        mark_dirty_locked(key);
    }
    wake_.notify_all();
}

void WorldScene::set_origin(int32_t x, int32_t y, int32_t z) {
    std::lock_guard lock(mutex_);
    if (origin_[0] == x && origin_[1] == y && origin_[2] == z) {
        return;
    }
    origin_[0] = x;
    origin_[1] = y;
    origin_[2] = z;
    origin_dirty_ = true;
    wake_.notify_all();
}

void WorldScene::set_chunk(ChunkKey key, std::shared_ptr<const ChunkVoxels> voxels, int lod) {
    std::lock_guard lock(mutex_);
    Chunk& chunk = chunks_[key];
    const std::shared_ptr<const ChunkVoxels> previous = std::move(chunk.voxels);
    const int previous_lod = chunk.lod;
    chunk.voxels = std::move(voxels);
    chunk.lod = lod > 0 ? 1 : 0;
    ++chunk.version;
    mark_dirty_locked(key);
    if (previous == nullptr || previous_lod != chunk.lod) {
        mark_neighbours_dirty_locked(key);
    } else {
        // An edit: only neighbours whose shared layer changed.
        for (std::size_t f = 0; f < 6; ++f) {
            const ChunkKey n = offset(key, kFaceOffsets[f]);
            const auto it = chunks_.find(n);
            if (it != chunks_.end() && it->second.voxels != nullptr && boundary_differs(*previous, *chunk.voxels, f)) {
                dirty_.insert(n);
            }
        }
    }
    wake_.notify_all();
}

void WorldScene::remove_chunk(ChunkKey key) {
    std::lock_guard lock(mutex_);
    const auto it = chunks_.find(key);
    if (it == chunks_.end() || it->second.voxels == nullptr) {
        return;
    }
    it->second.voxels.reset();
    ++it->second.version;
    mark_dirty_locked(key);
    mark_neighbours_dirty_locked(key);
    wake_.notify_all();
}

void WorldScene::clear() {
    std::lock_guard lock(mutex_);
    for (auto& [key, chunk] : chunks_) {
        chunk.voxels.reset();
        ++chunk.version;
        mark_dirty_locked(key);
    }
    wake_.notify_all();
}

bool WorldScene::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] { return dirty_.empty() && !busy_ && !origin_dirty_; });
}

void WorldScene::mark_dirty_locked(ChunkKey key) { dirty_.insert(key); }

void WorldScene::mark_neighbours_dirty_locked(ChunkKey key) {
    for (const ChunkKey& d : kFaceOffsets) {
        const ChunkKey n = offset(key, d);
        const auto it = chunks_.find(n);
        if (it != chunks_.end() && it->second.voxels != nullptr) {
            dirty_.insert(n);
        }
    }
}

IPLMatrix4x4 WorldScene::transform_locked(ChunkKey key) const {
    IPLMatrix4x4 m{};
    m.elements[0][0] = 1.0f;
    m.elements[1][1] = 1.0f;
    m.elements[2][2] = 1.0f;
    m.elements[3][3] = 1.0f;
    // Row-major, translation in the last column. Chunk positions relative to the origin are small.
    m.elements[0][3] = static_cast<float>(static_cast<int64_t>(key.x) * kChunkSize - origin_[0]);
    m.elements[1][3] = static_cast<float>(static_cast<int64_t>(key.y) * kChunkSize - origin_[1]);
    m.elements[2][3] = static_cast<float>(static_cast<int64_t>(key.z) * kChunkSize - origin_[2]);
    return m;
}

void WorldScene::worker_main() {
    struct Job {
        ChunkKey key;
        uint64_t version = 0;
        std::shared_ptr<const ChunkVoxels> voxels;
        std::array<std::shared_ptr<const ChunkVoxels>, 6> neighbours;
        int lod = 0;
        std::unique_ptr<Built> built;  // the result; null when the chunk is empty or removed
        double ms = 0.0;
    };

    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stop_ || !dirty_.empty() || origin_dirty_; });
        if (stop_) {
            return;
        }
        busy_ = true;

        // Take a batch and everything it needs, then mesh without the lock.
        std::vector<Job> jobs;
        while (!dirty_.empty() && jobs.size() < kBatch) {
            const ChunkKey key = *dirty_.begin();
            dirty_.erase(dirty_.begin());
            const auto it = chunks_.find(key);
            if (it == chunks_.end()) {
                continue;
            }
            Job job;
            job.key = key;
            job.version = it->second.version;
            job.voxels = it->second.voxels;
            job.lod = it->second.lod;
            for (std::size_t f = 0; f < 6; ++f) {
                const auto n = chunks_.find(offset(key, kFaceOffsets[f]));
                if (n != chunks_.end()) {
                    job.neighbours[f] = n->second.voxels;
                }
            }
            jobs.push_back(std::move(job));
        }
        const std::shared_ptr<const Mesher> mesher = mesher_;
        const std::vector<IPLMaterial> materials = ipl_materials_;
        const bool move_origin = origin_dirty_;
        origin_dirty_ = false;
        lock.unlock();

        for (Job& job : jobs) {
            if (job.voxels == nullptr) {
                continue;  // removal
            }
            const auto start = std::chrono::steady_clock::now();
            Neighbours neighbours{};
            for (std::size_t f = 0; f < 6; ++f) {
                neighbours[f] = job.neighbours[f].get();
            }
            auto mesh = std::make_shared<ChunkMesh>(mesher->mesh(*job.voxels, neighbours, job.lod));
            if (mesh->triangle_count() > 0 && !materials.empty()) {
                try {
                    auto built = std::make_unique<Built>();
                    built->mesh = mesh;
                    built->lod = job.lod;
                    IPLSceneSettings settings = steam_.scene_settings();
                    check(iplSceneCreate(steam_.context(), &settings, built->sub.out()), "iplSceneCreate");
                    std::vector<IPLint32> material_indices(mesh->materials.begin(), mesh->materials.end());
                    IPLStaticMeshSettings mesh_settings{};
                    mesh_settings.numVertices = static_cast<IPLint32>(mesh->vertex_count());
                    mesh_settings.numTriangles = static_cast<IPLint32>(mesh->triangle_count());
                    mesh_settings.numMaterials = static_cast<IPLint32>(materials.size());
                    mesh_settings.vertices = reinterpret_cast<IPLVector3*>(const_cast<float*>(mesh->vertices.data()));
                    mesh_settings.triangles =
                        reinterpret_cast<IPLTriangle*>(const_cast<int32_t*>(mesh->triangles.data()));
                    mesh_settings.materialIndices = material_indices.data();
                    mesh_settings.materials = const_cast<IPLMaterial*>(materials.data());
                    check(iplStaticMeshCreate(built->sub.get(), &mesh_settings, built->static_mesh.out()),
                          "iplStaticMeshCreate");
                    iplStaticMeshAdd(built->static_mesh.get(), built->sub.get());
                    iplSceneCommit(built->sub.get());
                    job.built = std::move(built);
                } catch (const Error& e) {
                    Log::writef(VSA_LOG_ERROR, "scene: chunk %d,%d,%d: %s", job.key.x, job.key.y, job.key.z, e.what());
                }
            }
            job.ms = since_ms(start);
        }

        // Swap the results in: the top-level scene changes under its lock, with one commit.
        std::vector<std::unique_ptr<Built>> retired;
        double commit_ms = 0.0;
        {
            std::scoped_lock both(scene_mutex_, mutex_);
            for (Job& job : jobs) {
                const auto it = chunks_.find(job.key);
                if (it == chunks_.end() || it->second.version != job.version) {
                    continue;  // changed again meanwhile; a newer job follows
                }
                Chunk& chunk = it->second;
                if (chunk.built && chunk.built->in_top) {
                    iplInstancedMeshRemove(chunk.built->instance.get(), top_.get());
                    chunk.built->in_top = false;
                }
                retired.push_back(std::move(chunk.built));
                if (job.built) {
                    IPLInstancedMeshSettings instance{};
                    instance.subScene = job.built->sub.get();
                    instance.transform = transform_locked(job.key);
                    if (iplInstancedMeshCreate(top_.get(), &instance, job.built->instance.out()) == IPL_STATUS_SUCCESS) {
                        iplInstancedMeshAdd(job.built->instance.get(), top_.get());
                        job.built->in_top = true;
                        chunk.built = std::move(job.built);
                    } else {
                        Log::writef(VSA_LOG_ERROR, "scene: chunk %d,%d,%d: iplInstancedMeshCreate failed", job.key.x,
                                    job.key.y, job.key.z);
                    }
                }
                ++stats_.chunks_built;
                if (chunk.built) {
                    chunk.built->version = static_cast<uint32_t>(stats_.chunks_built);
                }
                stats_.last_build_ms = job.ms;
                stats_.max_build_ms = std::max(stats_.max_build_ms, job.ms);
                if (chunk.voxels == nullptr) {
                    retired.push_back(std::move(chunk.built));
                    chunks_.erase(it);
                }
            }
            if (move_origin) {
                for (const auto& [key, chunk] : chunks_) {
                    if (chunk.built && chunk.built->in_top) {
                        iplInstancedMeshUpdateTransform(chunk.built->instance.get(), top_.get(), transform_locked(key));
                    }
                }
            }
            const auto start = std::chrono::steady_clock::now();
            iplSceneCommit(top_.get());
            commit_ms = since_ms(start);
            stats_.last_commit_ms = commit_ms;
        }
        retired.clear();  // after the commit that dropped them from the top scene

        lock.lock();
        busy_ = false;
        if (dirty_.empty() && !origin_dirty_) {
            idle_.notify_all();
        }
    }
}

SceneStats WorldScene::stats() const {
    std::lock_guard lock(mutex_);
    SceneStats s = stats_;
    s.chunks = 0;
    s.meshed_chunks = 0;
    s.triangles = 0;
    s.vertices = 0;
    s.memory_bytes = 0;
    for (const auto& [key, chunk] : chunks_) {
        if (chunk.voxels != nullptr) {
            ++s.chunks;
            s.memory_bytes += chunk.voxels->materials.size() * sizeof(uint16_t);
        }
        if (chunk.built) {
            ++s.meshed_chunks;
            const ChunkMesh& mesh = *chunk.built->mesh;
            s.triangles += mesh.triangle_count();
            s.vertices += mesh.vertex_count();
            s.memory_bytes += mesh.vertices.size() * sizeof(float) + mesh.triangles.size() * sizeof(int32_t) +
                              mesh.materials.size() * sizeof(uint16_t);
        }
    }
    s.pending_chunks = static_cast<uint32_t>(dirty_.size()) + (busy_ ? 1u : 0u);
    std::copy_n(origin_, 3, s.origin);
    return s;
}

bool WorldScene::chunk_mesh(ChunkKey key, std::shared_ptr<const ChunkMesh>& mesh, int& lod) const {
    uint32_t version = 0;
    return chunk_mesh(key, mesh, lod, version);
}

bool WorldScene::chunk_mesh(ChunkKey key, std::shared_ptr<const ChunkMesh>& mesh, int& lod, uint32_t& version) const {
    std::lock_guard lock(mutex_);
    const auto it = chunks_.find(key);
    if (it == chunks_.end() || !it->second.built) {
        return false;
    }
    mesh = it->second.built->mesh;
    lod = it->second.built->lod;
    version = it->second.built->version;
    return true;
}

std::vector<ChunkKey> WorldScene::chunk_keys() const {
    std::lock_guard lock(mutex_);
    std::vector<ChunkKey> keys;
    for (const auto& [key, chunk] : chunks_) {
        if (chunk.voxels != nullptr) {
            keys.push_back(key);
        }
    }
    return keys;
}

bool WorldScene::raycast(const float origin[3], const float direction[3], float max_distance, RayHit& hit) const {
    const double dir[3] = {static_cast<double>(direction[0]), static_cast<double>(direction[1]),
                           static_cast<double>(direction[2])};
    const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-9) || !(max_distance > 0.0f)) {
        return false;
    }
    const double o[3] = {static_cast<double>(origin[0]), static_cast<double>(origin[1]), static_cast<double>(origin[2])};
    const double d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};

    std::vector<std::tuple<ChunkKey, std::shared_ptr<const ChunkMesh>, int>> meshes;
    int32_t scene_origin[3];
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, chunk] : chunks_) {
            if (chunk.built) {
                meshes.emplace_back(key, chunk.built->mesh, chunk.built->lod);
            }
        }
        std::copy_n(origin_, 3, scene_origin);
    }

    double best = static_cast<double>(max_distance);
    bool found = false;
    for (const auto& [key, mesh, lod] : meshes) {
        // Chunk bounds in scene coordinates: skip chunks the ray misses (slab test).
        const double lo[3] = {static_cast<double>(static_cast<int64_t>(key.x) * kChunkSize - scene_origin[0]),
                              static_cast<double>(static_cast<int64_t>(key.y) * kChunkSize - scene_origin[1]),
                              static_cast<double>(static_cast<int64_t>(key.z) * kChunkSize - scene_origin[2])};
        double t0 = 0.0;
        double t1 = best;
        for (int a = 0; a < 3 && t0 <= t1; ++a) {
            if (std::abs(d[a]) < 1e-12) {
                if (o[a] < lo[a] || o[a] > lo[a] + kChunkSize) {
                    t0 = 1.0;
                    t1 = 0.0;
                }
                continue;
            }
            double ta = (lo[a] - o[a]) / d[a];
            double tb = (lo[a] + kChunkSize - o[a]) / d[a];
            if (ta > tb) {
                std::swap(ta, tb);
            }
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (t0 > t1) {
            continue;
        }
        // Möller–Trumbore against every triangle, in chunk-local coordinates.
        const double lo_ray[3] = {o[0] - lo[0], o[1] - lo[1], o[2] - lo[2]};
        for (std::size_t t = 0; t < mesh->triangle_count(); ++t) {
            const auto vertex = [&](int k, int axis) {
                return static_cast<double>(
                    mesh->vertices[static_cast<std::size_t>(mesh->triangles[t * 3 + static_cast<std::size_t>(k)]) * 3 +
                                   static_cast<std::size_t>(axis)]);
            };
            const double e1[3] = {vertex(1, 0) - vertex(0, 0), vertex(1, 1) - vertex(0, 1), vertex(1, 2) - vertex(0, 2)};
            const double e2[3] = {vertex(2, 0) - vertex(0, 0), vertex(2, 1) - vertex(0, 1), vertex(2, 2) - vertex(0, 2)};
            const double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (std::abs(det) < 1e-12) {
                continue;
            }
            const double inv = 1.0 / det;
            const double s[3] = {lo_ray[0] - vertex(0, 0), lo_ray[1] - vertex(0, 1), lo_ray[2] - vertex(0, 2)};
            const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
            if (u < 0.0 || u > 1.0) {
                continue;
            }
            const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
            if (v < 0.0 || u + v > 1.0) {
                continue;
            }
            const double dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
            if (dist <= 1e-6 || dist >= best) {
                continue;
            }
            best = dist;
            found = true;
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int a = 0; a < 3; ++a) {
                n[a] /= nl;
                hit.point[a] = static_cast<float>(o[a] + d[a] * dist);
                hit.normal[a] = static_cast<float>(n[a]);
                // The source block sits just behind the surface's front.
                const double world = o[a] + d[a] * dist - n[a] * 0.01 + scene_origin[a];
                hit.cell[a] = static_cast<int32_t>(std::floor(world));
            }
            hit.distance = static_cast<float>(dist);
            hit.chunk = key;
            hit.triangle = static_cast<uint32_t>(t);
            hit.material = mesh->materials[t];
            hit.lod = lod;
            hit.from_partial = t >= mesh->first_partial_triangle;
        }
    }
    return found;
}

std::size_t WorldScene::material_count() const {
    std::lock_guard lock(mutex_);
    return materials_.size();
}

std::vector<AcousticMaterial> WorldScene::materials() const {
    std::lock_guard lock(mutex_);
    return materials_;
}

void WorldScene::save_obj(const std::string& path) const {
    std::vector<std::pair<ChunkKey, std::shared_ptr<const ChunkMesh>>> meshes;
    std::vector<AcousticMaterial> materials;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, chunk] : chunks_) {
            if (chunk.built) {
                meshes.emplace_back(key, chunk.built->mesh);
            }
        }
        materials = materials_;
    }
    const auto name_of = [&](uint16_t m) {
        return m < materials.size() && !materials[m].name.empty() ? materials[m].name : "material" + std::to_string(m);
    };

    std::string mtl_path = path;
    const std::size_t dot = mtl_path.find_last_of('.');
    mtl_path = (dot == std::string::npos ? mtl_path : mtl_path.substr(0, dot)) + ".mtl";
    const std::size_t slash = mtl_path.find_last_of("/\\");
    const std::string mtl_name = slash == std::string::npos ? mtl_path : mtl_path.substr(slash + 1);

    std::ofstream obj(path);
    std::ofstream mtl(mtl_path);
    if (!obj || !mtl) {
        throw Error(VSA_ERROR_INVALID_ARGUMENT, "cannot write '" + path + "'");
    }
    mtl << "# Acoustic materials (colours are arbitrary)\n";
    for (std::size_t m = 0; m < materials.size(); ++m) {
        const uint32_t h = static_cast<uint32_t>(m) * 2654435761u;
        mtl << "newmtl " << name_of(static_cast<uint16_t>(m)) << "\nKd " << ((h >> 8) & 255) / 255.0 << ' '
            << ((h >> 16) & 255) / 255.0 << ' ' << ((h >> 24) & 255) / 255.0 << "\n";
    }

    obj << "# Vintage Story Steam Audio scene: " << meshes.size() << " chunks, world block coordinates\n";
    obj << "mtllib " << mtl_name << "\n" << std::fixed << std::setprecision(3);
    std::size_t base = 1;
    for (const auto& [key, mesh] : meshes) {
        obj << "g chunk_" << key.x << '_' << key.y << '_' << key.z << "\n";
        const double ox = static_cast<double>(key.x) * kChunkSize;
        const double oy = static_cast<double>(key.y) * kChunkSize;
        const double oz = static_cast<double>(key.z) * kChunkSize;
        for (std::size_t v = 0; v < mesh->vertex_count(); ++v) {
            obj << "v " << ox + static_cast<double>(mesh->vertices[v * 3]) << ' '
                << oy + static_cast<double>(mesh->vertices[v * 3 + 1]) << ' '
                << oz + static_cast<double>(mesh->vertices[v * 3 + 2]) << "\n";
        }
        int current = -1;
        for (std::size_t t = 0; t < mesh->triangle_count(); ++t) {
            if (mesh->materials[t] != current) {
                current = mesh->materials[t];
                obj << "usemtl " << name_of(mesh->materials[t]) << "\n";
            }
            obj << "f " << base + static_cast<std::size_t>(mesh->triangles[t * 3]) << ' '
                << base + static_cast<std::size_t>(mesh->triangles[t * 3 + 1]) << ' '
                << base + static_cast<std::size_t>(mesh->triangles[t * 3 + 2]) << "\n";
        }
        base += mesh->vertex_count();
    }
    if (!obj) {
        throw Error(VSA_ERROR_INTERNAL, "writing '" + path + "' failed");
    }
}

}  // namespace vsa::world

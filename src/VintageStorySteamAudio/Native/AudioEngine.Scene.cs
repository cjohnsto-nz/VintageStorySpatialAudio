using System.Runtime.InteropServices;
using System.Text;

namespace VintageStorySteamAudio.Native;

/// <summary>An acoustic material for the world scene. Bands are low, mid, high.</summary>
public sealed record AcousticMaterialDesc(
    string Name,
    MaterialKind Kind,
    (float Low, float Mid, float High) Absorption,
    float Scattering,
    (float Low, float Mid, float High) Transmission,
    (float Low, float Mid, float High) AttenuationDbPerMetre);

/// <summary>
/// One chunk's acoustic contents: a material id per cell (index (y * 32 + z) * 32 + x; partial
/// blocks' cells hold 0) plus the partial blocks' boxes. Reusable: <see cref="Clear"/> keeps the arrays.
/// </summary>
public sealed class ChunkSnapshot
{
    public int X { get; set; }

    public int Y { get; set; }

    public int Z { get; set; }

    /// <summary>0 = full detail, 1 = 2³-block super-voxels.</summary>
    public int Lod { get; set; }

    public ushort[] Materials { get; } = new ushort[VsaNative.ChunkCells];

    public List<VsaPartialBlock> Partials { get; } = [];

    public List<VsaBox> Boxes { get; } = [];

    public void Clear()
    {
        Array.Clear(Materials);
        Partials.Clear();
        Boxes.Clear();
    }

    /// <summary>Adds a partial block at a cell with its boxes (block units, 0..1).</summary>
    public void AddPartial(int cell, ushort material, IEnumerable<VsaBox> boxes)
    {
        ArgumentNullException.ThrowIfNull(boxes);
        int first = Boxes.Count;
        Boxes.AddRange(boxes);
        Partials.Add(new VsaPartialBlock
        {
            Cell = (uint)cell,
            Material = material,
            FirstBox = (uint)first,
            BoxCount = (uint)(Boxes.Count - first),
        });
    }

    /// <summary>A 64-bit fingerprint of the contents, to skip re-sending unchanged chunks.</summary>
    public ulong Fingerprint()
    {
        ulong hash = 0xCBF29CE484222325ul ^ (ulong)Lod;
        hash = Mix(hash, MemoryMarshal.AsBytes(Materials.AsSpan()));
        hash = Mix(hash, MemoryMarshal.AsBytes(CollectionsMarshal.AsSpan(Partials)));
        return Mix(hash, MemoryMarshal.AsBytes(CollectionsMarshal.AsSpan(Boxes)));
    }

    private static ulong Mix(ulong hash, ReadOnlySpan<byte> bytes)
    {
        ReadOnlySpan<ulong> words = MemoryMarshal.Cast<byte, ulong>(bytes);
        foreach (ulong w in words)
        {
            hash = (hash ^ w) * 0x100000001B3ul;
            hash ^= hash >> 29;
        }

        for (int i = words.Length * sizeof(ulong); i < bytes.Length; i++)
        {
            hash = (hash ^ bytes[i]) * 0x100000001B3ul;
        }

        return hash;
    }
}

public sealed record SceneStats(
    int Chunks,
    int MeshedChunks,
    int PendingChunks,
    long Triangles,
    long Vertices,
    long MemoryBytes,
    long ChunksBuilt,
    double LastBuildMs,
    double MaxBuildMs,
    double LastCommitMs,
    (int X, int Y, int Z) Origin,
    int MaterialCount);

/// <summary>A ray's first hit on the acoustic scene.</summary>
/// <param name="Distance">Metres along the ray.</param>
/// <param name="Point">Scene coordinates (relative to the origin).</param>
/// <param name="Normal">The surface's front (its open side).</param>
/// <param name="Chunk">The chunk whose mesh was hit.</param>
/// <param name="Triangle">Index into that chunk's mesh.</param>
/// <param name="Material">Acoustic material id.</param>
/// <param name="FromPartial">A partial block's box rather than a whole cell's face.</param>
/// <param name="Cell">World block that produced the surface.</param>
/// <param name="Lod">The chunk's level of detail.</param>
public sealed record SceneRayHit(
    float Distance,
    (float X, float Y, float Z) Point,
    (float X, float Y, float Z) Normal,
    (int X, int Y, int Z) Chunk,
    int Triangle,
    ushort Material,
    bool FromPartial,
    (int X, int Y, int Z) Cell,
    int Lod);

/// <summary>A chunk's mesh as submitted to Steam Audio, in chunk-local block units.</summary>
public sealed record ChunkMeshData(int Lod, uint Version, float[] Vertices, int[] Triangles, ushort[] Materials);

public sealed partial class AudioEngine
{
    /// <summary>Sets the material table (id = index; id 0 must be air). Re-meshes every chunk.</summary>
    public unsafe void SetSceneMaterials(IReadOnlyList<AcousticMaterialDesc> materials)
    {
        ArgumentNullException.ThrowIfNull(materials);
        using Lease lease = new(handle);
        var names = materials.Select(m => Encoding.UTF8.GetBytes((m.Name ?? string.Empty) + "\0")).ToArray();
        var handles = names.Select(n => GCHandle.Alloc(n, GCHandleType.Pinned)).ToArray();
        try
        {
            var native = new VsaAcousticMaterial[materials.Count];
            for (int i = 0; i < materials.Count; i++)
            {
                AcousticMaterialDesc m = materials[i];
                ref VsaAcousticMaterial n = ref native[i];
                n.StructSize = (uint)sizeof(VsaAcousticMaterial);
                n.Kind = (uint)m.Kind;
                n.Absorption[0] = m.Absorption.Low;
                n.Absorption[1] = m.Absorption.Mid;
                n.Absorption[2] = m.Absorption.High;
                n.Scattering = m.Scattering;
                n.Transmission[0] = m.Transmission.Low;
                n.Transmission[1] = m.Transmission.Mid;
                n.Transmission[2] = m.Transmission.High;
                n.AttenuationDbPerMetre[0] = m.AttenuationDbPerMetre.Low;
                n.AttenuationDbPerMetre[1] = m.AttenuationDbPerMetre.Mid;
                n.AttenuationDbPerMetre[2] = m.AttenuationDbPerMetre.High;
                n.Name = (byte*)handles[i].AddrOfPinnedObject();
            }

            fixed (VsaAcousticMaterial* table = native)
            {
                NativeException.ThrowIfFailed(VsaNative.SceneSetMaterials(lease.Engine, table, (uint)native.Length), "vsa_scene_set_materials");
            }
        }
        finally
        {
            foreach (var h in handles)
            {
                h.Free();
            }
        }
    }

    /// <summary>Adds or replaces a chunk (copied; meshed asynchronously).</summary>
    public unsafe void SetSceneChunk(ChunkSnapshot chunk)
    {
        ArgumentNullException.ThrowIfNull(chunk);
        using Lease lease = new(handle);
        Span<VsaPartialBlock> partials = CollectionsMarshal.AsSpan(chunk.Partials);
        Span<VsaBox> boxes = CollectionsMarshal.AsSpan(chunk.Boxes);
        fixed (ushort* materials = chunk.Materials)
        fixed (VsaPartialBlock* p = partials)
        fixed (VsaBox* b = boxes)
        {
            var desc = new VsaChunkDesc
            {
                StructSize = (uint)sizeof(VsaChunkDesc),
                X = chunk.X,
                Y = chunk.Y,
                Z = chunk.Z,
                Lod = (uint)chunk.Lod,
                Materials = materials,
                Partials = p,
                PartialCount = (uint)partials.Length,
                BoxCount = (uint)boxes.Length,
                Boxes = b,
            };
            NativeException.ThrowIfFailed(VsaNative.SceneSetChunk(lease.Engine, in desc), "vsa_scene_set_chunk");
        }
    }

    public void RemoveSceneChunk(int x, int y, int z)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.SceneRemoveChunk(lease.Engine, x, y, z), "vsa_scene_remove_chunk");
    }

    public void ClearScene()
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.SceneClear(lease.Engine), "vsa_scene_clear");
    }

    /// <summary>Block position the engine's positions are relative to.</summary>
    public void SetSceneOrigin(int x, int y, int z)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.SceneSetOrigin(lease.Engine, x, y, z), "vsa_scene_set_origin");
    }

    /// <summary>Waits for pending chunks to be meshed. False on timeout.</summary>
    public bool WaitSceneIdle(TimeSpan timeout)
    {
        using Lease lease = new(handle);
        VsaResult result = VsaNative.SceneWaitIdle(lease.Engine, (uint)Math.Clamp(timeout.TotalMilliseconds, 0, uint.MaxValue));
        if (result == VsaResult.InvalidState)
        {
            return false;
        }

        NativeException.ThrowIfFailed(result, "vsa_scene_wait_idle");
        return true;
    }

    public unsafe SceneStats GetSceneStats()
    {
        using Lease lease = new(handle);
        var s = new VsaSceneStats { StructSize = (uint)sizeof(VsaSceneStats) };
        NativeException.ThrowIfFailed(VsaNative.SceneGetStats(lease.Engine, ref s), "vsa_scene_get_stats");
        return new SceneStats(
            (int)s.Chunks, (int)s.MeshedChunks, (int)s.PendingChunks, (long)s.Triangles, (long)s.Vertices,
            (long)s.MemoryBytes, (long)s.ChunksBuilt, s.LastBuildMs, s.MaxBuildMs, s.LastCommitMs,
            (s.Origin[0], s.Origin[1], s.Origin[2]), (int)s.MaterialCount);
    }

    /// <summary>The chunk's mesh version, or null if it has no mesh. Cheap: no geometry is copied.</summary>
    public unsafe uint? GetChunkMeshVersion(int x, int y, int z)
    {
        using Lease lease = new(handle);
        var mesh = new VsaChunkMesh { StructSize = (uint)sizeof(VsaChunkMesh) };
        NativeException.ThrowIfFailed(VsaNative.SceneGetChunkMesh(lease.Engine, x, y, z, ref mesh), "vsa_scene_get_chunk_mesh");
        return mesh.Found != 0 ? mesh.Version : null;
    }

    /// <summary>The chunk's mesh exactly as submitted to Steam Audio, or null if it has none.</summary>
    public unsafe ChunkMeshData? GetChunkMesh(int x, int y, int z)
    {
        using Lease lease = new(handle);
        for (int attempt = 0; attempt < 4; attempt++)
        {
            var mesh = new VsaChunkMesh { StructSize = (uint)sizeof(VsaChunkMesh) };
            NativeException.ThrowIfFailed(VsaNative.SceneGetChunkMesh(lease.Engine, x, y, z, ref mesh), "vsa_scene_get_chunk_mesh");
            if (mesh.Found == 0)
            {
                return null;
            }

            // Sized from the query; if the chunk is re-meshed in between, the counts grow: retry.
            var vertices = new float[mesh.VertexCount * 3];
            var triangles = new int[mesh.TriangleCount * 3];
            var materials = new ushort[mesh.TriangleCount];
            fixed (float* v = vertices)
            fixed (int* t = triangles)
            fixed (ushort* m = materials)
            {
                mesh.VertexCapacity = mesh.VertexCount;
                mesh.TriangleCapacity = mesh.TriangleCount;
                mesh.Vertices = v;
                mesh.Triangles = t;
                mesh.Materials = m;
                uint vertexCount = mesh.VertexCount;
                uint triangleCount = mesh.TriangleCount;
                NativeException.ThrowIfFailed(VsaNative.SceneGetChunkMesh(lease.Engine, x, y, z, ref mesh), "vsa_scene_get_chunk_mesh");
                if (mesh.Found != 0 && mesh.VertexCount == vertexCount && mesh.TriangleCount == triangleCount)
                {
                    return new ChunkMeshData((int)mesh.Lod, mesh.Version, vertices, triangles, materials);
                }
            }
        }

        return null;
    }

    /// <summary>The chunks the scene holds.</summary>
    public unsafe IReadOnlyList<(int X, int Y, int Z)> ListSceneChunks()
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.SceneListChunks(lease.Engine, null, 0, out uint count), "vsa_scene_list_chunks");
        var keys = new int[(count + 16) * 3];
        fixed (int* k = keys)
        {
            NativeException.ThrowIfFailed(VsaNative.SceneListChunks(lease.Engine, k, count + 16, out count), "vsa_scene_list_chunks");
        }

        int n = (int)Math.Min(count, (uint)(keys.Length / 3));
        var result = new List<(int, int, int)>(n);
        for (int i = 0; i < n; i++)
        {
            result.Add((keys[i * 3], keys[(i * 3) + 1], keys[(i * 3) + 2]));
        }

        return result;
    }

    /// <summary>The first surface of the acoustic scene along a ray (scene coordinates), or null.</summary>
    public unsafe SceneRayHit? RaycastScene((float X, float Y, float Z) origin, (float X, float Y, float Z) direction, float maxDistance)
    {
        using Lease lease = new(handle);
        var hit = new VsaRayHit { StructSize = (uint)sizeof(VsaRayHit) };
        float* o = stackalloc float[3] { origin.X, origin.Y, origin.Z };
        float* d = stackalloc float[3] { direction.X, direction.Y, direction.Z };
        NativeException.ThrowIfFailed(VsaNative.SceneRaycast(lease.Engine, o, d, maxDistance, ref hit), "vsa_scene_raycast");
        if (hit.Hit == 0)
        {
            return null;
        }

        return new SceneRayHit(
            hit.Distance,
            (hit.Point[0], hit.Point[1], hit.Point[2]),
            (hit.Normal[0], hit.Normal[1], hit.Normal[2]),
            (hit.Chunk[0], hit.Chunk[1], hit.Chunk[2]),
            (int)hit.Triangle,
            (ushort)hit.Material,
            hit.FromPartial != 0,
            (hit.Cell[0], hit.Cell[1], hit.Cell[2]),
            (int)hit.Lod);
    }

    /// <summary>Writes the scene as OBJ + MTL in world block coordinates.</summary>
    public unsafe void SaveSceneObj(string path)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        using Lease lease = new(handle);
        byte[] utf8 = Encoding.UTF8.GetBytes(path + "\0");
        fixed (byte* p = utf8)
        {
            NativeException.ThrowIfFailed(VsaNative.SceneSaveObj(lease.Engine, p), "vsa_scene_save_obj");
        }
    }
}

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

/// <summary>One simulated source's latest direct-simulation results.</summary>
/// <param name="Voice">The voice handle.</param>
/// <param name="Position">Scene coordinates.</param>
/// <param name="SimulatedPosition">Where it was simulated from (moved out of a solid block if <paramref name="Escaped"/>).</param>
/// <param name="Escaped">Moved out of the block it sits in.</param>
/// <param name="Occlusion">Visible fraction, 0..1.</param>
/// <param name="Transmission">Amplitude per band through what is in the way.</param>
/// <param name="SolidMetres">Metres of material on the centre line.</param>
/// <param name="Crossings">Materials the centre line entered.</param>
public sealed record SourceDebugInfo(
    ulong Voice,
    (float X, float Y, float Z) Position,
    (float X, float Y, float Z) SimulatedPosition,
    bool Escaped,
    float Occlusion,
    (float Low, float Mid, float High) Transmission,
    float SolidMetres,
    int Crossings)
{
    /// <summary>The direct sound's gain per band: the visible part plus what passes through.</summary>
    public (float Low, float Mid, float High) Gain =>
        (Occlusion + ((1 - Occlusion) * Transmission.Low),
         Occlusion + ((1 - Occlusion) * Transmission.Mid),
         Occlusion + ((1 - Occlusion) * Transmission.High));
}

/// <summary>The direct simulation's latest tick.</summary>
/// <param name="Sources">Sources simulated.</param>
/// <param name="Ticks">Ticks so far.</param>
/// <param name="LastTickMs">The latest tick's duration.</param>
/// <param name="MaxTickMs">The longest tick so far.</param>
/// <param name="OcclusionMs">The latest tick's Steam Audio occlusion part.</param>
/// <param name="TransmissionMs">The latest tick's voxel transmission part.</param>
/// <param name="RateHz">Ticks per second while a device plays.</param>
/// <param name="OcclusionSamples">Rays per source.</param>
/// <param name="Listener">Where the latest tick listened from (scene coordinates).</param>
/// <param name="Origin">The scene origin the latest tick used.</param>
public sealed record SimulationStats(
    int Sources,
    long Ticks,
    double LastTickMs,
    double MaxTickMs,
    double OcclusionMs,
    double TransmissionMs,
    int RateHz,
    int OcclusionSamples,
    (float X, float Y, float Z) Listener,
    (int X, int Y, int Z) Origin);

/// <summary>The reflection simulation (Phase 6), for debugging views.</summary>
/// <param name="Enabled">Whether reflections run.</param>
/// <param name="Slots">Voices that can have reflections of their own.</param>
/// <param name="LiveSlots">Voices hearing their own reflections now.</param>
/// <param name="WaitingSlots">Voices given a slot, waiting for its first simulation.</param>
/// <param name="DrainingSlots">Slots letting a stopped voice's tail die away.</param>
/// <param name="Rays">Rays per simulation.</param>
/// <param name="Bounces">Bounces per ray.</param>
/// <param name="Order">Ambisonic order.</param>
/// <param name="RateHz">Simulations per second at most.</param>
/// <param name="Threads">Worker threads per simulation.</param>
/// <param name="DurationSeconds">Impulse response length.</param>
/// <param name="TransitionSeconds">Convolved part of each response.</param>
/// <param name="Ticks">Simulations so far.</param>
/// <param name="LastTickMs">The latest simulation's duration.</param>
/// <param name="MaxTickMs">The longest so far.</param>
/// <param name="SimulateMs">The latest simulation's Steam Audio part.</param>
/// <param name="ListenerReverbTimes">Decay time (RT60, s) where you are: below 800 Hz, to 8 kHz, above.</param>
/// <param name="OutputDb">Level of the reflections, dB full scale (-120 = silent).</param>
/// <param name="Gain">The reflection gain in use.</param>
/// <param name="Listener">Where the latest simulation listened from (scene coordinates).</param>
public sealed record ReflectionStats(
    bool Enabled,
    int Slots,
    int LiveSlots,
    int WaitingSlots,
    int DrainingSlots,
    int Rays,
    int Bounces,
    int Order,
    int RateHz,
    int Threads,
    float DurationSeconds,
    float TransitionSeconds,
    long Ticks,
    double LastTickMs,
    double MaxTickMs,
    double SimulateMs,
    (float Low, float Mid, float High) ListenerReverbTimes,
    float OutputDb,
    float Gain,
    (float X, float Y, float Z) Listener);

/// <summary>One simulated reflection source.</summary>
/// <param name="Slot">0: the listener's reverb, which every other sound shares; otherwise a voice's own.</param>
/// <param name="Voice">The voice (0 for the listener's reverb).</param>
/// <param name="Position">Simulated from (scene coordinates).</param>
/// <param name="ReverbTimes">Decay time per band, seconds.</param>
/// <param name="Eq">The tail's starting level per band.</param>
/// <param name="Delay">Samples before the tail starts.</param>
public sealed record ReflectionSourceInfo(
    int Slot,
    ulong Voice,
    (float X, float Y, float Z) Position,
    (float Low, float Mid, float High) ReverbTimes,
    (float Low, float Mid, float High) Eq,
    int Delay);

/// <summary>One leg of a traced sound path (scene coordinates).</summary>
/// <param name="Bounce">0 for the leg leaving the origin.</param>
/// <param name="From">Start.</param>
/// <param name="To">End: a surface, or where the leg ran out.</param>
/// <param name="Energy">Mid-band energy left on arrival (1 at the origin).</param>
/// <param name="Material">The material at <paramref name="To"/>; 0 if the leg ended in the open.</param>
public readonly record struct RaySegment(int Bounce, (float X, float Y, float Z) From, (float X, float Y, float Z) To, float Energy, ushort Material);

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

    /// <summary>The sources the direct simulation handled in its latest tick.</summary>
    public unsafe IReadOnlyList<SourceDebugInfo> GetSimulatedSources()
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.EngineGetSources(lease.Engine, null, 0, out uint count), "vsa_engine_get_sources");
        if (count == 0)
        {
            return [];
        }

        var buffer = new VsaSourceDebug[count + 8];
        buffer[0].StructSize = (uint)sizeof(VsaSourceDebug);
        fixed (VsaSourceDebug* p = buffer)
        {
            NativeException.ThrowIfFailed(VsaNative.EngineGetSources(lease.Engine, p, (uint)buffer.Length, out count), "vsa_engine_get_sources");
        }

        var result = new List<SourceDebugInfo>((int)Math.Min(count, (uint)buffer.Length));
        for (int i = 0; i < result.Capacity; i++)
        {
            ref VsaSourceDebug d = ref buffer[i];
            result.Add(new SourceDebugInfo(
                d.Voice,
                (d.Position[0], d.Position[1], d.Position[2]),
                (d.SimulatedPosition[0], d.SimulatedPosition[1], d.SimulatedPosition[2]),
                (d.Flags & VsaNative.SourceEscaped) != 0,
                d.Occlusion,
                (d.Transmission[0], d.Transmission[1], d.Transmission[2]),
                d.SolidMetres,
                (int)d.Crossings));
        }

        return result;
    }

    public unsafe SimulationStats GetSimulationStats()
    {
        using Lease lease = new(handle);
        var s = new VsaSimulationStats { StructSize = (uint)sizeof(VsaSimulationStats) };
        NativeException.ThrowIfFailed(VsaNative.EngineGetSimulationStats(lease.Engine, ref s), "vsa_engine_get_simulation_stats");
        return new SimulationStats(
            (int)s.Sources, (long)s.Ticks, s.LastTickMs, s.MaxTickMs, s.OcclusionMs, s.TransmissionMs, (int)s.RateHz, (int)s.OcclusionSamples,
            (s.ListenerX, s.ListenerY, s.ListenerZ), (s.OriginX, s.OriginY, s.OriginZ));
    }

    public unsafe ReflectionStats GetReflectionStats()
    {
        using Lease lease = new(handle);
        var s = new VsaReflectionStats { StructSize = (uint)sizeof(VsaReflectionStats) };
        NativeException.ThrowIfFailed(VsaNative.EngineGetReflectionStats(lease.Engine, ref s), "vsa_engine_get_reflection_stats");
        return new ReflectionStats(
            s.Enabled != 0, (int)s.Slots, (int)s.LiveSlots, (int)s.WaitingSlots, (int)s.DrainingSlots,
            (int)s.Rays, (int)s.Bounces, (int)s.Order, (int)s.RateHz, (int)s.Threads, s.Duration, s.Transition,
            (long)s.Ticks, s.LastTickMs, s.MaxTickMs, s.SimulateMs,
            (s.ListenerReverbTimes[0], s.ListenerReverbTimes[1], s.ListenerReverbTimes[2]),
            s.OutputDb, s.Gain, (s.Listener[0], s.Listener[1], s.Listener[2]));
    }

    /// <summary>The sources the reflection simulation handled in its latest run (the listener's first).</summary>
    public unsafe IReadOnlyList<ReflectionSourceInfo> GetReflectionSources()
    {
        using Lease lease = new(handle);
        var buffer = new VsaReflectionSource[65];
        buffer[0].StructSize = (uint)sizeof(VsaReflectionSource);
        uint count;
        fixed (VsaReflectionSource* p = buffer)
        {
            NativeException.ThrowIfFailed(
                VsaNative.EngineGetReflectionSources(lease.Engine, p, (uint)buffer.Length, out count), "vsa_engine_get_reflection_sources");
        }

        int n = (int)Math.Min(count, (uint)buffer.Length);
        var result = new List<ReflectionSourceInfo>(n);
        for (int i = 0; i < n; i++)
        {
            ref VsaReflectionSource d = ref buffer[i];
            result.Add(new ReflectionSourceInfo(
                (int)d.Slot,
                d.Voice,
                (d.Position[0], d.Position[1], d.Position[2]),
                (d.ReverbTimes[0], d.ReverbTimes[1], d.ReverbTimes[2]),
                (d.Eq[0], d.Eq[1], d.Eq[2]),
                d.Delay));
        }

        return result;
    }

    /// <summary>Scales every reflection (smoothly): 1 = as simulated; 0..4.</summary>
    public void SetReflectionGain(float gain)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.EngineSetReflectionGain(lease.Engine, gain), "vsa_engine_set_reflection_gain");
    }

    /// <summary>
    /// Debugging: sound paths from <paramref name="origin"/> (scene coordinates) bouncing off the
    /// voxel world, <paramref name="rays"/> directions and up to <paramref name="bounces"/> reflections.
    /// </summary>
    public unsafe IReadOnlyList<RaySegment> TraceRays((float X, float Y, float Z) origin, int rays, int bounces, float maxDistance)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(rays);
        ArgumentOutOfRangeException.ThrowIfNegative(bounces);
        using Lease lease = new(handle);
        var buffer = new VsaRaySegment[Math.Max(1, rays * (bounces + 1))];
        buffer[0].StructSize = (uint)sizeof(VsaRaySegment);
        float* o = stackalloc float[3] { origin.X, origin.Y, origin.Z };
        uint count;
        fixed (VsaRaySegment* p = buffer)
        {
            NativeException.ThrowIfFailed(
                VsaNative.SceneTraceRays(lease.Engine, o, (uint)rays, (uint)bounces, maxDistance, p, (uint)buffer.Length, out count),
                "vsa_scene_trace_rays");
        }

        var result = new RaySegment[count];
        for (int i = 0; i < result.Length; i++)
        {
            ref VsaRaySegment s = ref buffer[i];
            result[i] = new RaySegment(
                (int)s.Bounce, (s.From[0], s.From[1], s.From[2]), (s.To[0], s.To[1], s.To[2]), s.Energy, (ushort)s.Material);
        }

        return result;
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

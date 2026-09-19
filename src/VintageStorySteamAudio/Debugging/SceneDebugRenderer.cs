using Vintagestory.API.Client;
using Vintagestory.API.MathTools;
using Vintagestory.Client.NoObf;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio.Debugging;

/// <summary>What the scene overlay draws.</summary>
[Flags]
public enum SceneOverlay
{
    None = 0,

    /// <summary>Every edge of the acoustic mesh, coloured by material.</summary>
    Wireframe = 1,

    /// <summary>The acoustic mesh's triangles, translucent and coloured by material.</summary>
    Faces = 2,

    /// <summary>Chunk outlines coloured by state: full detail, coarse, not meshed.</summary>
    Bounds = 4,
}

/// <summary>
/// Draws the world scene exactly as Steam Audio has it: each chunk's mesh is read back from the
/// engine (<see cref="AudioEngine.GetChunkMesh"/>) and re-uploaded only when its version changes.
/// Uses the game's own wireframe shader, as its debug wireframes do, with each chunk placed
/// relative to the camera in double precision.
/// </summary>
internal sealed class SceneDebugRenderer : IRenderer
{
    private const long RefreshMs = 500;

    private readonly ICoreClientAPI capi;
    private readonly AudioEngine engine;
    private readonly Func<MaterialTable?> materials;
    private readonly Func<ChunkKey?> centre;
    private readonly Dictionary<ChunkKey, Cached> cache = [];
    private readonly Matrixf matrix = new();
    private readonly WireframeCube box;
    private readonly Vec4f white = new(1f, 1f, 1f, 1f);
    private readonly Vec4f faceTint = new(1f, 1f, 1f, 0.35f);
    private readonly Vec4f fullColor = new(0.3f, 1f, 0.3f, 0.8f);
    private readonly Vec4f coarseColor = new(0.3f, 0.6f, 1f, 0.6f);
    private readonly Vec4f emptyColor = new(0.6f, 0.6f, 0.6f, 0.35f);
    private long nextRefresh;
    private bool disposed;

    public SceneDebugRenderer(ICoreClientAPI capi, AudioEngine engine, Func<MaterialTable?> materials, Func<ChunkKey?> centre)
    {
        this.capi = capi;
        this.engine = engine;
        this.materials = materials;
        this.centre = centre;
        box = WireframeCube.CreateUnitCube(capi, -1);
    }

    public SceneOverlay Overlay { get; set; }

    /// <summary>Chunks drawn around the listener's (horizontally and vertically).</summary>
    public int Radius { get; set; } = 1;

    public double RenderOrder => 0.9;

    public int RenderRange => 999;

    /// <summary>Triangles currently shown (for the HUD).</summary>
    public long TrianglesShown { get; private set; }

    public void OnRenderFrame(float deltaTime, EnumRenderStage stage)
    {
        if (Overlay == SceneOverlay.None || capi.World.Player?.Entity is not { } player || centre() is not { } c)
        {
            return;
        }

        long now = Environment.TickCount64;
        if (now >= nextRefresh)
        {
            nextRefresh = now + RefreshMs;
            Refresh(c);
        }

        Vec3d camera = player.CameraPos;
        IShaderProgram program = capi.Shader.GetProgram((int)EnumShaderProgram.Wireframe);
        long shown = 0;
        foreach ((ChunkKey key, Cached cached) in cache)
        {
            double ox = key.X * (double)VsaNative.ChunkSize;
            double oy = key.Y * (double)VsaNative.ChunkSize;
            double oz = key.Z * (double)VsaNative.ChunkSize;
            if ((Overlay & SceneOverlay.Bounds) != 0)
            {
                Vec4f color = cached.Lines is null ? emptyColor : cached.Lod == 0 ? fullColor : coarseColor;
                box.Render(capi, ox, oy, oz, VsaNative.ChunkSize, VsaNative.ChunkSize, VsaNative.ChunkSize, 1.2f, color);
            }

            if (cached.Lines is null)
            {
                continue;
            }

            matrix.Identity().Set(capi.Render.CameraMatrixOrigin).Translate(ox - camera.X, oy - camera.Y, oz - camera.Z);
            program.Use();
            capi.Render.GLEnableDepthTest();
            capi.Render.GLDepthMask(on: false);
            capi.Render.GlToggleBlend(blend: true);
            program.Uniform("origin", 0f, 0f, 0f);
            program.UniformMatrix("projectionMatrix", capi.Render.CurrentProjectionMatrix);
            program.UniformMatrix("modelViewMatrix", matrix.Values);
            if ((Overlay & SceneOverlay.Faces) != 0 && cached.Faces is not null)
            {
                program.Uniform("colorIn", faceTint);
                capi.Render.RenderMesh(cached.Faces);
            }

            if ((Overlay & SceneOverlay.Wireframe) != 0)
            {
                capi.Render.LineWidth = 1.4f;
                program.Uniform("colorIn", white);
                capi.Render.RenderMesh(cached.Lines);
                capi.Render.LineWidth = 1.6f;
            }

            program.Stop();
            capi.Render.GLDepthMask(on: true);
            shown += cached.Triangles;
        }

        TrianglesShown = shown;
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        Clear();
        box.Dispose();
    }

    /// <summary>Drops every cached mesh (they are rebuilt on the next refresh).</summary>
    public void Clear()
    {
        foreach (Cached cached in cache.Values)
        {
            cached.Dispose();
        }

        cache.Clear();
        TrianglesShown = 0;
    }

    private void Refresh(ChunkKey c)
    {
        MaterialTable? table = materials();
        var wanted = new HashSet<ChunkKey>();
        for (int dy = -Radius; dy <= Radius; dy++)
        {
            for (int dz = -Radius; dz <= Radius; dz++)
            {
                for (int dx = -Radius; dx <= Radius; dx++)
                {
                    wanted.Add(new ChunkKey(c.X + dx, c.Y + dy, c.Z + dz));
                }
            }
        }

        foreach (ChunkKey key in cache.Keys.Where(k => !wanted.Contains(k)).ToList())
        {
            cache[key].Dispose();
            cache.Remove(key);
        }

        foreach (ChunkKey key in wanted)
        {
            uint? version = engine.GetChunkMeshVersion(key.X, key.Y, key.Z);
            if (cache.TryGetValue(key, out Cached? existing) && existing.Version == version)
            {
                continue;
            }

            existing?.Dispose();
            ChunkMeshData? mesh = version is null ? null : engine.GetChunkMesh(key.X, key.Y, key.Z);
            cache[key] = mesh is null ? new Cached(null, null, 0, version, 0) : Build(mesh, table);
        }
    }

    private Cached Build(ChunkMeshData mesh, MaterialTable? table)
    {
        int triangles = mesh.Materials.Length;
        var lines = new MeshData(triangles * 6, triangles * 6, withNormals: false, withUv: false, withRgba: true, withFlags: true);
        lines.SetMode(EnumDrawMode.Lines);
        var faces = new MeshData(triangles * 3, triangles * 3, withNormals: false, withUv: false, withRgba: true, withFlags: true);
        faces.SetMode(EnumDrawMode.Triangles);
        for (int t = 0; t < triangles; t++)
        {
            int argb = table is not null && mesh.Materials[t] < table.Colors.Count ? table.Colors[mesh.Materials[t]] : unchecked((int)0xFFFF00FF);
            int rgba = ToRgba(argb, 255);
            int translucent = ToRgba(argb, 160);
            for (int k = 0; k < 3; k++)
            {
                int a = mesh.Triangles[(t * 3) + k] * 3;
                int b = mesh.Triangles[(t * 3) + ((k + 1) % 3)] * 3;
                lines.AddVertexSkipTex(mesh.Vertices[a], mesh.Vertices[a + 1], mesh.Vertices[a + 2], rgba);
                lines.AddIndex(lines.VerticesCount - 1);
                lines.AddVertexSkipTex(mesh.Vertices[b], mesh.Vertices[b + 1], mesh.Vertices[b + 2], rgba);
                lines.AddIndex(lines.VerticesCount - 1);
                faces.AddVertexSkipTex(mesh.Vertices[a], mesh.Vertices[a + 1], mesh.Vertices[a + 2], translucent);
                faces.AddIndex(faces.VerticesCount - 1);
            }
        }

        // The wireframe shader pulls flagged vertices towards the camera: the acoustic surfaces lie
        // exactly on the terrain's.
        lines.Flags = Enumerable.Repeat(256, lines.VerticesCount).ToArray();
        faces.Flags = Enumerable.Repeat(256, faces.VerticesCount).ToArray();
        return new Cached(capi.Render.UploadMesh(lines), capi.Render.UploadMesh(faces), mesh.Lod, mesh.Version, triangles);
    }

    /// <summary>0xAARRGGBB to the bytes R, G, B, A that MeshData.AddVertexSkipTex stores.</summary>
    private static int ToRgba(int argb, int alpha) =>
        ((argb >> 16) & 0xFF) | (((argb >> 8) & 0xFF) << 8) | ((argb & 0xFF) << 16) | (alpha << 24);

    private sealed record Cached(MeshRef? Lines, MeshRef? Faces, int Lod, uint? Version, long Triangles) : IDisposable
    {
        public void Dispose()
        {
            Lines?.Dispose();
            Faces?.Dispose();
        }
    }
}

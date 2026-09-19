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

    /// <summary>
    /// A line from the listener to every simulated sound, coloured by what reaches the listener:
    /// green clear, through yellow (-20 dB) to red (-40 dB and below); a white stub where a sound
    /// was moved out of the block it sits in.
    /// </summary>
    Sources = 8,

    /// <summary>
    /// Sound paths from the listener bouncing off the scene (the reflections' view of the world),
    /// bright cyan fading to dark blue as each surface absorbs energy; and a magenta line to each
    /// voice with reflections of its own.
    /// </summary>
    Reflections = 16,
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
    private readonly Vec4f probeColor = new(1f, 0.9f, 0.1f, 1f);
    private readonly Vec4f probeFill = new(1f, 1f, 1f, 0.55f);
    private long nextRefresh;
    private bool disposed;
    private SceneRayHit? probe;
    private MeshRef? probeLines;
    private MeshRef? probeFace;
    private MeshRef? sourceLines;
    private Vec3d sourceAnchor = new();
    private MeshRef? rayLines;
    private MeshRef? slotLines;
    private Vec3d rayAnchor = new();

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

    /// <summary>
    /// The direct simulation's sources, drawn as lines from <paramref name="from"/> (world
    /// coordinates, near the listener). Positions are scene coordinates relative to <paramref name="origin"/>.
    /// </summary>
    public void SetSources(IReadOnlyList<SourceDebugInfo> sources, (int X, int Y, int Z) origin, Vec3d from)
    {
        sourceLines?.Dispose();
        sourceLines = null;
        if (sources.Count == 0)
        {
            return;
        }

        sourceAnchor = from.Clone();
        var lines = new MeshData(sources.Count * 10, sources.Count * 10, withNormals: false, withUv: false, withRgba: true, withFlags: true);
        lines.SetMode(EnumDrawMode.Lines);
        void Line(double ax, double ay, double az, double bx, double by, double bz, int rgba)
        {
            lines.AddVertexSkipTex((float)(ax - sourceAnchor.X), (float)(ay - sourceAnchor.Y), (float)(az - sourceAnchor.Z), rgba);
            lines.AddIndex(lines.VerticesCount - 1);
            lines.AddVertexSkipTex((float)(bx - sourceAnchor.X), (float)(by - sourceAnchor.Y), (float)(bz - sourceAnchor.Z), rgba);
            lines.AddIndex(lines.VerticesCount - 1);
        }

        foreach (SourceDebugInfo s in sources)
        {
            double px = s.Position.X + (double)origin.X;
            double py = s.Position.Y + (double)origin.Y;
            double pz = s.Position.Z + (double)origin.Z;
            double sx = s.SimulatedPosition.X + (double)origin.X;
            double sy = s.SimulatedPosition.Y + (double)origin.Y;
            double sz = s.SimulatedPosition.Z + (double)origin.Z;
            int color = ToRgba(GainColor(s.Gain.Mid), 255);
            Line(from.X, from.Y, from.Z, sx, sy, sz, color);
            const double c = 0.25;  // a small cross at the source
            Line(sx - c, sy, sz, sx + c, sy, sz, color);
            Line(sx, sy - c, sz, sx, sy + c, sz, color);
            Line(sx, sy, sz - c, sx, sy, sz + c, color);
            if (s.Escaped)
            {
                Line(px, py, pz, sx, sy, sz, ToRgba(unchecked((int)0xFFFFFFFF), 255));
            }
        }

        lines.Flags = Enumerable.Repeat(256, lines.VerticesCount).ToArray();
        sourceLines = capi.Render.UploadMesh(lines);
    }

    /// <summary>
    /// The reflections view: traced sound paths (scene coordinates relative to
    /// <paramref name="origin"/>), and lines from <paramref name="from"/> (world coordinates) to each
    /// voice with reflections of its own.
    /// </summary>
    public void SetReflections(IReadOnlyList<RaySegment> rays, IReadOnlyList<ReflectionSourceInfo> slots, (int X, int Y, int Z) origin, Vec3d from)
    {
        rayLines?.Dispose();
        rayLines = null;
        slotLines?.Dispose();
        slotLines = null;
        rayAnchor = from.Clone();
        if (rays.Count > 0)
        {
            var lines = new MeshData(rays.Count * 2, rays.Count * 2, withNormals: false, withUv: false, withRgba: true, withFlags: true);
            lines.SetMode(EnumDrawMode.Lines);
            foreach (RaySegment r in rays)
            {
                // The energy on leaving the previous surface (at the start) and on arrival.
                int start = ToRgba(EnergyColor(r.Bounce == 0 ? 1f : r.Energy / Math.Max(1e-3f, 1f - 0.1f)), EnergyAlpha(r.Energy));
                int end = ToRgba(EnergyColor(r.Energy), EnergyAlpha(r.Energy));
                lines.AddVertexSkipTex((float)(r.From.X + origin.X - rayAnchor.X), (float)(r.From.Y + origin.Y - rayAnchor.Y), (float)(r.From.Z + origin.Z - rayAnchor.Z), start);
                lines.AddIndex(lines.VerticesCount - 1);
                lines.AddVertexSkipTex((float)(r.To.X + origin.X - rayAnchor.X), (float)(r.To.Y + origin.Y - rayAnchor.Y), (float)(r.To.Z + origin.Z - rayAnchor.Z), end);
                lines.AddIndex(lines.VerticesCount - 1);
            }

            lines.Flags = Enumerable.Repeat(256, lines.VerticesCount).ToArray();
            rayLines = capi.Render.UploadMesh(lines);
        }

        var voices = slots.Where(s => s.Slot > 0).ToList();
        if (voices.Count > 0)
        {
            var lines = new MeshData(voices.Count * 8, voices.Count * 8, withNormals: false, withUv: false, withRgba: true, withFlags: true);
            lines.SetMode(EnumDrawMode.Lines);
            int magenta = ToRgba(unchecked((int)0xFFFF40FF), 255);
            void Line(double ax, double ay, double az, double bx, double by, double bz)
            {
                lines.AddVertexSkipTex((float)(ax - rayAnchor.X), (float)(ay - rayAnchor.Y), (float)(az - rayAnchor.Z), magenta);
                lines.AddIndex(lines.VerticesCount - 1);
                lines.AddVertexSkipTex((float)(bx - rayAnchor.X), (float)(by - rayAnchor.Y), (float)(bz - rayAnchor.Z), magenta);
                lines.AddIndex(lines.VerticesCount - 1);
            }

            foreach (ReflectionSourceInfo s in voices)
            {
                double x = s.Position.X + (double)origin.X;
                double y = s.Position.Y + (double)origin.Y;
                double z = s.Position.Z + (double)origin.Z;
                Line(from.X, from.Y, from.Z, x, y, z);
                const double c = 0.35;  // a diamond at the source
                Line(x - c, y, z, x, y + c, z);
                Line(x, y + c, z, x + c, y, z);
                Line(x + c, y, z, x, y - c, z);
            }

            lines.Flags = Enumerable.Repeat(256, lines.VerticesCount).ToArray();
            slotLines = capi.Render.UploadMesh(lines);
        }
    }

    /// <summary>Bright cyan (all the energy) to dark blue (a thousandth), as 0xAARRGGBB.</summary>
    public static int EnergyColor(float energy)
    {
        double t = Math.Clamp(-Math.Log10(Math.Max(energy, 1e-3f)) / 3.0, 0.0, 1.0);
        int red = (int)Math.Round(140 * (1 - t) + (30 * t));
        int green = (int)Math.Round(255 * (1 - t) + (40 * t));
        int blue = (int)Math.Round(255 * (1 - t) + (170 * t));
        return unchecked((int)0xFF000000) | (red << 16) | (green << 8) | blue;
    }

    private static int EnergyAlpha(float energy) => (int)Math.Round(255 * Math.Clamp(0.25 + (0.75 * Math.Sqrt(Math.Max(energy, 0f))), 0.0, 1.0));

    /// <summary>Green (0 dB) through yellow (-20 dB) to red (-40 dB and below), as 0xAARRGGBB.</summary>
    public static int GainColor(float gain)
    {
        double db = 20.0 * Math.Log10(Math.Max(gain, 1e-6f));
        double t = Math.Clamp(-db / 40.0, 0.0, 1.0);
        int red = (int)Math.Round(255 * Math.Min(1.0, t * 2));
        int green = (int)Math.Round(255 * Math.Min(1.0, 2 - (t * 2)));
        return unchecked((int)0xFF000000) | (red << 16) | (green << 8) | 40;
    }

    /// <summary>The ray probe's hit, highlighted: the triangle it hit and the block that produced it.</summary>
    public void SetProbe(SceneRayHit? hit)
    {
        if (hit is not null && probe is not null && hit.Chunk == probe.Chunk && hit.Triangle == probe.Triangle && probeLines is not null)
        {
            probe = hit;
            return;
        }

        probeLines?.Dispose();
        probeFace?.Dispose();
        probeLines = null;
        probeFace = null;
        probe = hit;
        if (hit is null || engine.GetChunkMesh(hit.Chunk.X, hit.Chunk.Y, hit.Chunk.Z) is not { } mesh || hit.Triangle * 3 + 2 >= mesh.Triangles.Length)
        {
            return;
        }

        var lines = new MeshData(6, 6, withNormals: false, withUv: false, withRgba: true, withFlags: true);
        lines.SetMode(EnumDrawMode.Lines);
        var face = new MeshData(3, 3, withNormals: false, withUv: false, withRgba: true, withFlags: true);
        face.SetMode(EnumDrawMode.Triangles);
        int yellow = ToRgba(unchecked((int)0xFFFFE619), 255);
        for (int k = 0; k < 3; k++)
        {
            int a = mesh.Triangles[(hit.Triangle * 3) + k] * 3;
            int b = mesh.Triangles[(hit.Triangle * 3) + ((k + 1) % 3)] * 3;
            lines.AddVertexSkipTex(mesh.Vertices[a], mesh.Vertices[a + 1], mesh.Vertices[a + 2], yellow);
            lines.AddIndex(lines.VerticesCount - 1);
            lines.AddVertexSkipTex(mesh.Vertices[b], mesh.Vertices[b + 1], mesh.Vertices[b + 2], yellow);
            lines.AddIndex(lines.VerticesCount - 1);
            face.AddVertexSkipTex(mesh.Vertices[a], mesh.Vertices[a + 1], mesh.Vertices[a + 2], ToRgba(unchecked((int)0xFFFFE619), 120));
            face.AddIndex(face.VerticesCount - 1);
        }

        lines.Flags = Enumerable.Repeat(512, lines.VerticesCount).ToArray();
        face.Flags = Enumerable.Repeat(512, face.VerticesCount).ToArray();
        probeLines = capi.Render.UploadMesh(lines);
        probeFace = capi.Render.UploadMesh(face);
    }

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
        RenderProbe(camera, program);
        RenderSources(camera, program);
        RenderReflections(camera, program);
    }

    private void RenderReflections(Vec3d camera, IShaderProgram program)
    {
        if ((Overlay & SceneOverlay.Reflections) == 0 || (rayLines is null && slotLines is null))
        {
            return;
        }

        matrix.Identity().Set(capi.Render.CameraMatrixOrigin)
            .Translate(rayAnchor.X - camera.X, rayAnchor.Y - camera.Y, rayAnchor.Z - camera.Z);
        program.Use();
        capi.Render.GlToggleBlend(blend: true);
        program.Uniform("origin", 0f, 0f, 0f);
        program.UniformMatrix("projectionMatrix", capi.Render.CurrentProjectionMatrix);
        program.UniformMatrix("modelViewMatrix", matrix.Values);
        program.Uniform("colorIn", white);
        if (rayLines is not null)
        {
            // The paths stay in the space they are traced in: hidden behind its walls like the world.
            capi.Render.GLEnableDepthTest();
            capi.Render.GLDepthMask(on: false);
            capi.Render.LineWidth = 1.5f;
            capi.Render.RenderMesh(rayLines);
            capi.Render.GLDepthMask(on: true);
        }

        if (slotLines is not null)
        {
            capi.Render.GLDisableDepthTest();  // voices behind walls too
            capi.Render.LineWidth = 2.5f;
            capi.Render.RenderMesh(slotLines);
            capi.Render.GLEnableDepthTest();
        }

        capi.Render.LineWidth = 1.6f;
        program.Stop();
    }

    private void RenderSources(Vec3d camera, IShaderProgram program)
    {
        if ((Overlay & SceneOverlay.Sources) == 0 || sourceLines is null)
        {
            return;
        }

        matrix.Identity().Set(capi.Render.CameraMatrixOrigin)
            .Translate(sourceAnchor.X - camera.X, sourceAnchor.Y - camera.Y, sourceAnchor.Z - camera.Z);
        program.Use();
        capi.Render.GLDisableDepthTest();  // through walls: that is the point
        capi.Render.GlToggleBlend(blend: true);
        program.Uniform("origin", 0f, 0f, 0f);
        program.UniformMatrix("projectionMatrix", capi.Render.CurrentProjectionMatrix);
        program.UniformMatrix("modelViewMatrix", matrix.Values);
        program.Uniform("colorIn", white);
        capi.Render.LineWidth = 2f;
        capi.Render.RenderMesh(sourceLines);
        capi.Render.LineWidth = 1.6f;
        program.Stop();
        capi.Render.GLEnableDepthTest();
    }

    private void RenderProbe(Vec3d camera, IShaderProgram program)
    {
        if (probe is not { } hit)
        {
            return;
        }

        // The block that produced the surface, then the triangle itself.
        box.Render(capi, hit.Cell.X - 0.002, hit.Cell.Y - 0.002, hit.Cell.Z - 0.002, 1.004f, 1.004f, 1.004f, 3f, probeColor);
        if (probeLines is null)
        {
            return;
        }

        double ox = hit.Chunk.X * (double)VsaNative.ChunkSize;
        double oy = hit.Chunk.Y * (double)VsaNative.ChunkSize;
        double oz = hit.Chunk.Z * (double)VsaNative.ChunkSize;
        matrix.Identity().Set(capi.Render.CameraMatrixOrigin).Translate(ox - camera.X, oy - camera.Y, oz - camera.Z);
        program.Use();
        capi.Render.GLEnableDepthTest();
        capi.Render.GLDepthMask(on: false);
        capi.Render.GlToggleBlend(blend: true);
        program.Uniform("origin", 0f, 0f, 0f);
        program.UniformMatrix("projectionMatrix", capi.Render.CurrentProjectionMatrix);
        program.UniformMatrix("modelViewMatrix", matrix.Values);
        program.Uniform("colorIn", probeFill);
        capi.Render.RenderMesh(probeFace);
        capi.Render.LineWidth = 3f;
        program.Uniform("colorIn", white);
        capi.Render.RenderMesh(probeLines);
        capi.Render.LineWidth = 1.6f;
        program.Stop();
        capi.Render.GLDepthMask(on: true);
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        Clear();
        SetProbe(null);
        sourceLines?.Dispose();
        sourceLines = null;
        rayLines?.Dispose();
        rayLines = null;
        slotLines?.Dispose();
        slotLines = null;
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

using System.Globalization;
using System.Text;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio.Debugging;

/// <summary>
/// The world scene's debugging tools: the overlay (<see cref="SceneDebugRenderer"/>), its HUD, a
/// hotkey that cycles the overlay (Ctrl+F7 by default, rebindable in the controls), and the
/// .steamaudio scene commands.
/// </summary>
internal sealed class SceneDebugTools : IDisposable
{
    public const string HotkeyCode = "vssteamaudio-scene-overlay";

    private static readonly SceneOverlay[] Cycle =
    [
        SceneOverlay.None,
        SceneOverlay.Wireframe,
        SceneOverlay.Wireframe | SceneOverlay.Bounds,
        SceneOverlay.Faces | SceneOverlay.Wireframe,
    ];

    private readonly ICoreClientAPI capi;
    private readonly AudioEngine engine;
    private readonly WorldAcoustics world;
    private readonly SceneDebugRenderer renderer;
    private readonly SceneHud hud;
    private readonly long hudListener;
    private SceneRayHit? probe;
    private bool disposed;

    public SceneDebugTools(ICoreClientAPI capi, AudioEngine engine, WorldAcoustics world)
    {
        this.capi = capi;
        this.engine = engine;
        this.world = world;
        renderer = new SceneDebugRenderer(capi, engine, () => world.Materials, () => world.Status().Centre);
        capi.Event.RegisterRenderer(renderer, EnumRenderStage.Opaque, "vssteamaudio-scene");
        hud = new SceneHud(capi);
        hudListener = capi.Event.RegisterGameTickListener(_ => UpdateHud(), 100);
        capi.Input.RegisterHotKey(HotkeyCode, "Steam Audio: cycle the acoustic scene overlay", GlKeys.F7, HotkeyType.DevTool, ctrlPressed: true);
        capi.Input.SetHotKeyHandler(HotkeyCode, _ =>
        {
            int next = (Array.IndexOf(Cycle, renderer.Overlay) + 1) % Cycle.Length;
            SetOverlay(Cycle[next]);
            capi.ShowChatMessage($"Acoustic scene overlay: {Describe(renderer.Overlay)}");
            return true;
        });
    }

    public void SetOverlay(SceneOverlay overlay)
    {
        renderer.Overlay = overlay;
        if (overlay == SceneOverlay.None)
        {
            hud.TryClose();
            renderer.Clear();
            probe = null;
            renderer.SetProbe(null);
        }
        else
        {
            hud.TryOpen();
            UpdateHud();
        }
    }

    /// <summary>".steamaudio scene [wire|faces|bounds|off|hud|radius N|legend|export|reload]".</summary>
    public string Command(string? action, string? argument)
    {
        switch ((action ?? string.Empty).ToLowerInvariant())
        {
            case "":
            case "status":
                return StatusText();
            case "wire":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Wireframe);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "faces":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Faces);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "bounds":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Bounds);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "off":
                SetOverlay(SceneOverlay.None);
                return "Overlay off.";
            case "radius":
                if (!int.TryParse(argument, NumberStyles.Integer, CultureInfo.InvariantCulture, out int radius) || radius < 0 || radius > 4)
                {
                    return "Usage: .steamaudio scene radius <0-4> (chunks drawn around yours)";
                }

                renderer.Radius = radius;
                renderer.Clear();
                return $"Overlay radius: {radius} chunk(s).";
            case "legend":
                return Legend();
            case "export":
                string folder = capi.GetOrCreateDataPath("Logs");
                string path = Path.Combine(folder, $"vssteamaudio-scene-{DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture)}.obj");
                engine.SaveSceneObj(path);
                return $"Scene written to {path} (world block coordinates; open it in Blender or MeshLab).";
            case "reload":
                renderer.Clear();
                return world.Reload();
            default:
                return "Usage: .steamaudio scene [status|wire|faces|bounds|off|radius N|legend|export|reload]";
        }
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        capi.Event.UnregisterGameTickListener(hudListener);
        capi.Event.UnregisterRenderer(renderer, EnumRenderStage.Opaque);
        renderer.Dispose();
        hud.TryClose();
        hud.Dispose();
    }

    private void UpdateHud()
    {
        if (renderer.Overlay == SceneOverlay.None || !hud.IsOpened())
        {
            return;
        }

        try
        {
            UpdateProbe();
            hud.SetText(StatusText());
        }
        catch (NativeException)
        {
            // The engine is shutting down.
        }
    }

    /// <summary>Casts a ray from the camera along the view into the acoustic scene (what Steam Audio's rays would meet).</summary>
    private void UpdateProbe()
    {
        if (capi.World.Player?.Entity is not { } player)
        {
            return;
        }

        WorldStatus w = world.Status();
        Vintagestory.API.MathTools.Vec3d camera = player.CameraPos;
        Vintagestory.API.MathTools.Vec3f view = player.Pos.GetViewVector();
        probe = engine.RaycastScene(
            ((float)(camera.X - w.Origin.X), (float)(camera.Y - w.Origin.Y), (float)(camera.Z - w.Origin.Z)),
            (view.X, view.Y, view.Z),
            64f);
        renderer.SetProbe(probe);
    }

    private string ProbeText()
    {
        if (probe is not { } hit)
        {
            return "Acoustic ray: nothing within 64 m";
        }

        string material = world.Materials?.NameOf(hit.Material) ?? hit.Material.ToString(CultureInfo.InvariantCulture);
        string source = hit.FromPartial ? "a partial block's box" : "a whole block's face";
        string facing = Math.Abs(hit.Normal.Y) > 0.5f ? (hit.Normal.Y > 0 ? "up" : "down")
            : Math.Abs(hit.Normal.X) > 0.5f ? (hit.Normal.X > 0 ? "east" : "west")
            : hit.Normal.Z > 0 ? "south" : "north";
        var cell = new BlockPos(hit.Cell.X, hit.Cell.Y, hit.Cell.Z);
        return string.Create(
            CultureInfo.InvariantCulture,
            $"Acoustic ray: {hit.Distance:0.00} m, {material}, {source} facing {facing}; chunk {hit.Chunk.X},{hit.Chunk.Y},{hit.Chunk.Z} triangle {hit.Triangle}{(hit.Lod > 0 ? " (coarse ring)" : string.Empty)}\n" +
            $"  made by block {hit.Cell.X},{hit.Cell.Y},{hit.Cell.Z}: {world.DescribeCell(cell)}");
    }

    private string StatusText()
    {
        SceneStats s = engine.GetSceneStats();
        WorldStatus w = world.Status();
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Acoustic scene: {s.Chunks} chunks ({s.MeshedChunks} meshed, {s.PendingChunks} pending), ")
            .Append(CultureInfo.InvariantCulture, $"{s.Triangles / 1000.0:0.#}k triangles, {s.MemoryBytes / (1024.0 * 1024.0):0.#} MB\n")
            .Append(CultureInfo.InvariantCulture, $"Meshing: last {s.LastBuildMs:0.0} ms, max {s.MaxBuildMs:0.0} ms, commit {s.LastCommitMs:0.00} ms, {s.ChunksBuilt} builds\n")
            .Append(CultureInfo.InvariantCulture, $"Streaming: {w.Sent}/{w.Desired} chunks, {w.Dirty} dirty, {w.ChunksRead} read, {w.ChunksSent} sent, tick {w.LastTickMs:0.0} ms (read max {w.MaxReadMs:0.0} ms)\n")
            .Append(CultureInfo.InvariantCulture, $"Centre chunk {w.Centre?.X},{w.Centre?.Y},{w.Centre?.Z}, origin {w.Origin.X},{w.Origin.Y},{w.Origin.Z}; overlay {Describe(renderer.Overlay)}, radius {renderer.Radius}, {renderer.TrianglesShown / 1000.0:0.#}k triangles shown\n")
            .Append(CultureInfo.InvariantCulture, $"Materials: {w.MaterialCount} from {w.MaterialSource}");
        if (capi.World.Player?.CurrentBlockSelection?.Position is { } pos)
        {
            text.Append("\nGame selection: ").Append(world.DescribeBlock(pos));
        }

        text.Append('\n').Append(ProbeText());

        return text.ToString();
    }

    private string Legend()
    {
        if (world.Materials is not { } table)
        {
            return "No materials loaded yet.";
        }

        var text = new StringBuilder("Acoustic materials:");
        for (int id = 1; id < table.Count; id++)
        {
            int rgb = table.Colors[id] & 0xFFFFFF;
            AcousticMaterialDesc m = table.Materials[id];
            text.Append(CultureInfo.InvariantCulture, $"\n<font color=\"#{rgb:x6}\">■■■</font> {m.Name} ({m.Kind})");
        }

        return text.ToString();
    }

    private static string Describe(SceneOverlay overlay) => overlay == SceneOverlay.None ? "off" : overlay.ToString().ToLowerInvariant();
}

using System.Globalization;
using System.Text;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;
using VintageStorySteamAudio.Diagnostics;

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
        SceneOverlay.Sources,
        SceneOverlay.Sources | SceneOverlay.Wireframe,
        SceneOverlay.Reflections,
        SceneOverlay.Reflections | SceneOverlay.Wireframe,
        SceneOverlay.Paths,
        SceneOverlay.Paths | SceneOverlay.Wireframe,
    ];

    // Sound paths drawn by the reflections overlay.
    private const long LegsHeldMs = 20000;
    private IReadOnlyList<PathSegment> legs = [];
    private (int X, int Y, int Z) legsOrigin;
    private long legsAt;
    private bool inspecting;
    private IReadOnlyList<AudibleVoice> audible = [];
    private int soundPage = 1;
    private const int OverlayRays = 48;
    private const int OverlayBounces = 6;
    private const float OverlayRayMetres = 48f;

    private readonly ICoreClientAPI capi;
    private readonly AudioEngine engine;
    private readonly WorldAcoustics world;
    private readonly System.Func<ulong, string?> describeVoice;
    private IReadOnlyList<SourceDebugInfo> sources = [];
    private readonly SceneDebugRenderer renderer;
    private readonly SceneHud hud;
    private readonly long hudListener;
    private SceneRayHit? probe;
    private bool disposed;

    public SceneDebugTools(ICoreClientAPI capi, AudioEngine engine, WorldAcoustics world, System.Func<ulong, string?> describeVoice)
    {
        this.capi = capi;
        this.engine = engine;
        this.world = world;
        this.describeVoice = describeVoice;
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
            sources = [];
            renderer.SetSources(sources, default, new Vec3d());
            renderer.SetReflections([], [], default, new Vec3d());
            renderer.SetPaths([], default, new Vec3d());
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
            case "sources":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Sources);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "rays":
            case "reflections":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Reflections);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "report":
                return Report();
            case "paths":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Paths);
                return $"Overlay: {Describe(renderer.Overlay)}";
            case "sounds":
                if (string.Equals(argument, "off", StringComparison.OrdinalIgnoreCase))
                {
                    inspecting = false;
                    engine.SetInspect(false);
                    SetOverlay(renderer.Overlay & ~SceneOverlay.Sounds);
                    return "Sound inspector off.";
                }

                soundPage = int.TryParse(argument, NumberStyles.Integer, CultureInfo.InvariantCulture, out int page) && page > 0
                    ? page
                    : 1;
                inspecting = true;
                engine.SetInspect(true);
                SetOverlay(renderer.Overlay | SceneOverlay.Sounds);
                return "Sound inspector on: the panel lists what you can hear, loudest first, and each one is drawn "
                    + "in the world -- green in the clear, orange through walls, blue round a corner (with an arrow "
                    + "to where it comes in), magenta only its reflections. '.steamaudio scene sounds off' ends it.";
            case "off":
                SetOverlay(SceneOverlay.None);
                inspecting = false;
                engine.SetInspect(false);
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
                return "Usage: .steamaudio scene [status|sounds [page|off]|wire|faces|bounds|sources|rays|paths|off|radius N|legend|export|reload]";
        }
    }

    /// <summary>".steamaudio reverb [status|gain N|early N|tail N|rays]".</summary>
    public string ReverbCommand(string? action, string? argument)
    {
        switch ((action ?? string.Empty).ToLowerInvariant())
        {
            case "":
            case "status":
                return ReflectionsText(detailed: true);
            case "gain":
                if (!float.TryParse(argument, NumberStyles.Float, CultureInfo.InvariantCulture, out float gain) || !float.IsFinite(gain) || gain < 0f || gain > 4f)
                {
                    return "Usage: .steamaudio reverb gain <0-4> (1 = as simulated; ReflectionGain in the config sets it at start)";
                }

                engine.SetReflectionGain(gain);
                return string.Create(CultureInfo.InvariantCulture, $"Reflection gain {gain:0.##} ({20 * Math.Log10(Math.Max(gain, 1e-4f)):0.#} dB).");
            case "early":
            case "tail":
                if (!float.TryParse(argument, NumberStyles.Float, CultureInfo.InvariantCulture, out float part) || !float.IsFinite(part) || part < 0f || part > 4f)
                {
                    return $"Usage: .steamaudio reverb {action!.ToLowerInvariant()} <0-4> (1 = as simulated, 0 = off)";
                }

                bool early = action!.Equals("early", StringComparison.OrdinalIgnoreCase);
                engine.SetReflectionMix(early ? part : engine.ReflectionEarlyGain, early ? engine.ReflectionTailGain : part);
                return string.Create(CultureInfo.InvariantCulture, $"Early reflections {engine.ReflectionEarlyGain:0.##}, reverb tail {engine.ReflectionTailGain:0.##}.");
            case "rays":
                SetOverlay(renderer.Overlay ^ SceneOverlay.Reflections);
                return $"Overlay: {Describe(renderer.Overlay)}";
            default:
                return "Usage: .steamaudio reverb [status|gain N|early N|tail N|rays]";
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

    /// <summary>
    /// ".steamaudio scene sounds [page]": every sound that is sounding, loudest first, with which
    /// way it is reaching the listener. A page at a time, because the HUD cuts off what it cannot
    /// fit. The engine only works the numbers out while this is on.
    /// </summary>
    /// <summary>What the debugging commands (solo, mute) have changed, for the panel's heading; null: nothing.</summary>
    public Func<string?>? DebugState { get; init; }

    /// <summary>A sound was soloed: the inspector on, with the sounds and the legs of their way round drawn.</summary>
    public void ShowSolo()
    {
        inspecting = true;
        soundPage = 1;
        engine.SetInspect(true);
        SetOverlay(renderer.Overlay | SceneOverlay.Sounds | SceneOverlay.Paths);
    }

    private string SoundsText()
    {
        string? state = DebugState?.Invoke();
        if (audible.Count == 0)
        {
            return "No sound is playing" + (state is null ? string.Empty : $" [{state}]") + ".";
        }

        // What the direct simulation knows about each of them, by voice.
        var walls = new Dictionary<ulong, SourceDebugInfo>();
        foreach (SourceDebugInfo s in sources)
        {
            walls[s.Voice] = s;
        }

        // Two lines a sound, plus the heading: as many as the window can show.
        int perPage = Math.Clamp((hud.LineBudget - 2) / 2, 3, 40);
        int pages = (audible.Count + perPage - 1) / perPage;
        int shown = Math.Clamp(soundPage, 1, pages);
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Sounds: {audible.Count} playing, loudest first (page {shown} of {pages}");
        text.Append(pages > 1 ? ", .steamaudio scene sounds <page>)" : ")");
        if (state is not null)
        {
            text.Append(" [").Append(state).Append(']');
        }

        text.Append("\n     green in the clear, orange through walls, blue round a corner (arrow: where it comes in), magenta reflections only");
        foreach ((AudibleVoice v, int index) in audible
            .Select((v, i) => (v, i))
            .Skip((shown - 1) * perPage)
            .Take(perPage))
        {
            string name = describeVoice(v.Voice) ?? $"voice {v.Voice}";
            text.Append(CultureInfo.InvariantCulture, $"\n{index + 1,3}. {v.HeardDb,6:0.0} dB {v.Bus,-7} {name}");
            text.Append(v.Routes.HasFlag(SoundRoutes.HeadLocked)
                ? " (at your head)"
                : string.Create(CultureInfo.InvariantCulture, $" {v.Distance:0.0} m"));
            text.Append(CultureInfo.InvariantCulture, $"\n     arrives {v.Route}:");
            text.Append(CultureInfo.InvariantCulture, $" direct {Level(v.DirectDb)}");
            if (v.Routes.HasFlag(SoundRoutes.Path))
            {
                text.Append(CultureInfo.InvariantCulture, $", round {Level(v.PathDb)}");
            }

            if (v.Routes.HasFlag(SoundRoutes.Place))
            {
                text.Append(CultureInfo.InvariantCulture, $", reflections {Level(v.ReflectionDb)}");
            }

            if (walls.TryGetValue(v.Voice, out SourceDebugInfo? w))
            {
                text.Append(CultureInfo.InvariantCulture, $"; {w.Occlusion:0%} of it visible");
                if (w.Crossings > 0)
                {
                    (float low, float mid, float high) = w.Transmission;
                    text.Append(CultureInfo.InvariantCulture,
                        $", {w.SolidMetres:0.0} m through {w.Crossings} material(s), passing {Db(low)}/{Db(mid)}/{Db(high)} dB");
                }
            }
            else if (!v.Routes.HasFlag(SoundRoutes.HeadLocked))
            {
                text.Append(" (not simulated: too quiet, or no effect set to spare)");
            }
        }

        return text.ToString();

        static string Level(float db) => db <= -199f ? "-" : string.Create(CultureInfo.InvariantCulture, $"{db:0.0} dB");
        static string Db(float amplitude) => string.Create(
            CultureInfo.InvariantCulture, $"{(amplitude > 1e-9f ? 20.0 * Math.Log10(amplitude) : -200.0):0}");
    }

    private void UpdateHud()
    {
        if ((renderer.Overlay == SceneOverlay.None && !inspecting) || !hud.IsOpened())
        {
            return;
        }

        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.Hud);
        try
        {
            if (inspecting)
            {
                // The inspector's own panel: what is sounding and how it reaches the listener,
                // and nothing else, so that the lines it needs are not pushed off the bottom.
                UpdateSounds();
                UpdatePaths();  // the legs of the ways round: with one sound soloed, all its own
                hud.SetText(SoundsText());
                return;
            }

            UpdateProbe();
            UpdateSources();
            UpdateReflections();
            UpdatePaths();
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
        Vintagestory.API.MathTools.Vec3d camera = Eyes(player);
        Vintagestory.API.MathTools.Vec3f view = player.Pos.GetViewVector();
        probe = engine.RaycastScene(
            ((float)(camera.X - w.Origin.X), (float)(camera.Y - w.Origin.Y), (float)(camera.Z - w.Origin.Z)),
            (view.X, view.Y, view.Z),
            64f);
        renderer.SetProbe(probe);
    }

    /// <summary>The sounds the inspector lists, drawn in the world in the same colours.</summary>
    private void UpdateSounds()
    {
        audible = engine.GetAudible();
        if ((renderer.Overlay & SceneOverlay.Sounds) == 0 || capi.World.Player?.Entity is not { } player)
        {
            renderer.SetSounds([], default, new Vec3d());
            return;
        }

        // From just in front of and below the eyes, so the lines are visible rather than end-on.
        Vec3d camera = Eyes(player);
        Vec3f view = player.Pos.GetViewVector();
        var from = new Vec3d(camera.X + (view.X * 0.6), camera.Y + (view.Y * 0.6) - 0.35, camera.Z + (view.Z * 0.6));
        renderer.SetSounds(audible, world.Status().Origin, from);
    }

    private void UpdateSources()
    {
        if ((renderer.Overlay & SceneOverlay.Sources) == 0 || capi.World.Player?.Entity is not { } player)
        {
            sources = [];
            return;
        }

        sources = engine.GetSimulatedSources();
        // From just in front of and below the eyes, so the lines are visible rather than end-on.
        Vec3d camera = Eyes(player);
        Vec3f view = player.Pos.GetViewVector();
        var from = new Vec3d(camera.X + (view.X * 0.6), camera.Y + (view.Y * 0.6) - 0.35, camera.Z + (view.Z * 0.6));
        renderer.SetSources(sources, world.Status().Origin, from);
    }

    private void UpdateReflections()
    {
        if ((renderer.Overlay & SceneOverlay.Reflections) == 0 || capi.World.Player?.Entity is not { } player)
        {
            return;
        }

        (int X, int Y, int Z) o = world.Status().Origin;
        Vec3d camera = Eyes(player);
        IReadOnlyList<RaySegment> rays = engine.TraceRays(
            ((float)(camera.X - o.X), (float)(camera.Y - o.Y), (float)(camera.Z - o.Z)), OverlayRays, OverlayBounces, OverlayRayMetres);
        Vec3f view = player.Pos.GetViewVector();
        var from = new Vec3d(camera.X + (view.X * 0.6), camera.Y + (view.Y * 0.6) - 0.35, camera.Z + (view.Z * 0.6));
        renderer.SetReflections(rays, engine.GetReflectionSources(), o, from);
    }

    private void UpdatePaths()
    {
        if ((renderer.Overlay & SceneOverlay.Paths) == 0 || capi.World.Player?.Entity is not { } player)
        {
            return;
        }

        // The legs are held a while after the pathing stops asking (close to a sound, where it
        // is heard direct, there are none): long enough to walk up to where they ran.
        IReadOnlyList<PathSegment> now = engine.GetPathSegments();
        if (now.Count > 0)
        {
            legs = now;
            legsOrigin = world.Status().Origin;
            legsAt = Environment.TickCount64;
        }
        else if (Environment.TickCount64 - legsAt > LegsHeldMs)
        {
            legs = [];
        }

        renderer.SetPaths(legs, legsOrigin, player.CameraPos);
    }

    /// <summary>
    /// ".steamaudio scene report": everything about how the listed sounds are reaching the
    /// listener, written to the log -- each leg of the ways round with every block it passes
    /// through and what the acoustic scene makes of that block.
    /// </summary>
    private string Report()
    {
        if (capi.World.Player?.Entity is not { } player)
        {
            return "No player.";
        }

        var lines = new List<string>();
        (int X, int Y, int Z) o = world.Status().Origin;
        Vec3d eyes = Eyes(player);
        SimulationStats sim = engine.GetSimulationStats();
        PathingStats paths = engine.GetPathingStats();
        lines.Add(string.Create(CultureInfo.InvariantCulture, $"listener eyes {eyes.X:0.00},{eyes.Y:0.00},{eyes.Z:0.00}; simulated from {sim.Listener.X + sim.Origin.X:0.00},{sim.Listener.Y + sim.Origin.Y:0.00},{sim.Listener.Z + sim.Origin.Z:0.00}; state: {DebugState?.Invoke() ?? "everything heard"}"));
        lines.Add(string.Create(CultureInfo.InvariantCulture, $"pathing: {paths}"));

        IReadOnlyList<SourceDebugInfo> simulated = engine.GetSimulatedSources();
        foreach (AudibleVoice v in engine.GetAudible().Take(8))
        {
            var at = new Vec3d(v.Position.X + o.X, v.Position.Y + o.Y, v.Position.Z + o.Z);
            lines.Add(string.Create(CultureInfo.InvariantCulture, $"sound {describeVoice(v.Voice) ?? "?"} at {at.X:0.00},{at.Y:0.00},{at.Z:0.00} ({v.Distance:0.0} m): heard {v.HeardDb:0.0} dB, direct {v.DirectDb:0.0}, round {v.PathDb:0.0}, reflections {v.ReflectionDb:0.0}; {v.Route}"));
            SourceDebugInfo? s = simulated.FirstOrDefault(x => x.Voice == v.Voice);
            if (s is null)
            {
                lines.Add("  not among the simulated sources");
                continue;
            }

            var from = new Vec3d(s.SimulatedPosition.X + o.X, s.SimulatedPosition.Y + o.Y, s.SimulatedPosition.Z + o.Z);
            lines.Add(string.Create(CultureInfo.InvariantCulture, $"  simulated from {from.X:0.00},{from.Y:0.00},{from.Z:0.00}{(s.Escaped ? " (moved out of its block)" : string.Empty)}: visible {s.Occlusion:0.00}, {s.SolidMetres:0.00} m of material in {s.Crossings} crossings on the line to the listener"));
            lines.Add("  the straight line to the listener passes through:");
            AddCrossed(lines, from, eyes);
        }

        lines.Add(string.Create(CultureInfo.InvariantCulture, $"{legs.Count} legs of ways round ({(Environment.TickCount64 - legsAt) / 1000.0:0.0} s old):"));
        int n = 0;
        foreach (PathSegment leg in legs.Take(40))
        {
            var a = new Vec3d(leg.From.X + legsOrigin.X, leg.From.Y + legsOrigin.Y, leg.From.Z + legsOrigin.Z);
            var b = new Vec3d(leg.To.X + legsOrigin.X, leg.To.Y + legsOrigin.Y, leg.To.Z + legsOrigin.Z);
            lines.Add(string.Create(CultureInfo.InvariantCulture, $"leg {++n}: {a.X:0.00},{a.Y:0.00},{a.Z:0.00} -> {b.X:0.00},{b.Y:0.00},{b.Z:0.00} ({a.DistanceTo(b):0.0} m){(leg.Occluded ? " REJECTED (blocked in the live scene)" : " accepted")}"));
            AddCrossed(lines, a, b);
        }

        foreach (string line in lines)
        {
            capi.Logger.Notification("[vssteamaudio report] {0}", line);
        }

        return $"Report written to client-main.log ({lines.Count} lines, {legs.Count} legs).";
    }

    /// <summary>Every block the straight line from a to b passes through that is not air.</summary>
    private void AddCrossed(List<string> lines, Vec3d a, Vec3d b)
    {
        double length = a.DistanceTo(b);
        int steps = Math.Max(1, (int)(length / 0.05));
        (int, int, int) last = (int.MinValue, 0, 0);
        int found = 0;
        for (int i = 0; i <= steps && found < 24; i++)
        {
            double t = (double)i / steps;
            (int X, int Y, int Z) cell = ((int)Math.Floor(a.X + ((b.X - a.X) * t)), (int)Math.Floor(a.Y + ((b.Y - a.Y) * t)), (int)Math.Floor(a.Z + ((b.Z - a.Z) * t)));
            if (cell == last)
            {
                continue;
            }

            last = cell;
            if (world.DescribeCell(cell.X, cell.Y, cell.Z) is { } what)
            {
                found++;
                lines.Add(string.Create(CultureInfo.InvariantCulture, $"    {cell.X},{cell.Y},{cell.Z} ({length * t:0.0} m along): {what}"));
            }
        }

        if (found == 0)
        {
            lines.Add("    nothing but air");
        }
    }

    private string PathingText()
    {
        PathingStats p = engine.GetPathingStats();
        if (!p.Enabled)
        {
            return "Pathing: off (Pathing in " + Config.SteamAudioConfig.FileName + ")";
        }

        string bake = p.Baking ? "baking now" : p.BakeDue ? "bake due" : "baked";
        return string.Create(
            CultureInfo.InvariantCulture,
            $"Pathing: {p.Probes} probes {p.ProbeSpacing:0.0} m apart round {p.BoxCentre.X:0},{p.BoxCentre.Y:0},{p.BoxCentre.Z:0} " +
            $"({bake}; {p.Bakes} bakes, {p.CancelledBakes} abandoned, last {p.LastBakeMs:0} ms, max {p.MaxBakeMs:0} ms); " +
            $"{p.Found} of {p.Wanted} blocked sounds have a path ({p.Simulated} simulated), tick {p.LastTickMs:0.0} ms, {p.RateHz} Hz");
    }

    /// <summary>What kind of space a decay time suggests (for the HUD).</summary>
    internal static string DescribeSpace(float rt60) => rt60 switch
    {
        <= 0f => "not simulated yet",
        < 0.25f => "open or deadened",
        < 0.6f => "a small room",
        < 1.2f => "a large room",
        < 2.0f => "a hall",
        _ => "a cave or cathedral",
    };

    private string ReflectionsText(bool detailed)
    {
        ReflectionStats r = engine.GetReflectionStats();
        if (!r.Enabled)
        {
            return "Reflections: off (Reflections in " + Config.SteamAudioConfig.FileName + ")";
        }

        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Reflections: RT60 here {r.ListenerReverbTimes.Low:0.00}/{r.ListenerReverbTimes.Mid:0.00}/{r.ListenerReverbTimes.High:0.00} s ({DescribeSpace(r.ListenerReverbTimes.Mid)}), ")
            .Append(CultureInfo.InvariantCulture, $"level {r.OutputDb:0} dB, gain {r.Gain:0.##} (early {engine.ReflectionEarlyGain:0.##}, tail {engine.ReflectionTailGain:0.##})\n")
            .Append(CultureInfo.InvariantCulture, $"  {r.LiveSlots}/{r.Slots} places simulated ({r.WaitingSlots} waiting, {r.DrainingSlots} fading); ")
            .Append(CultureInfo.InvariantCulture, $"simulation {r.LastTickMs:0} ms (max {r.MaxTickMs:0}), {r.Rays} rays x {r.Bounces} bounces, {r.DurationSeconds:0.0} s, order {r.Order}, up to {r.RateHz} Hz on {r.Threads} threads");
        if (!detailed && (renderer.Overlay & SceneOverlay.Reflections) == 0)
        {
            return text.ToString();
        }

        foreach (ReflectionSourceInfo s in engine.GetReflectionSources().Where(s => s.Slot > 0).Take(8))
        {
            string name = describeVoice(s.Voice) ?? $"voice {s.Voice}";
            text.Append(CultureInfo.InvariantCulture, $"\n  at {name}: RT60 {s.ReverbTimes.Mid:0.00} s ({DescribeSpace(s.ReverbTimes.Mid)})");
        }

        return text.ToString();
    }

    private string SourcesText()
    {
        SimulationStats sim = engine.GetSimulationStats();
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture, $"Direct simulation: {sim.Sources} sources, tick {sim.LastTickMs:0.00} ms (occlusion {sim.OcclusionMs:0.00}, transmission {sim.TransmissionMs:0.00}), max {sim.MaxTickMs:0.0} ms, {sim.RateHz} Hz, {sim.OcclusionSamples} rays");
        if (capi.World.Player?.Entity is { } me)
        {
            // Frame check: where the simulation listens (world) against the eyes, and both origins.
            Vec3d eyes = Eyes(me);
            (int X, int Y, int Z) wo = world.Status().Origin;
            text.Append(CultureInfo.InvariantCulture, $"\n  listens at {sim.Listener.X + sim.Origin.X:0.0},{sim.Listener.Y + sim.Origin.Y:0.0},{sim.Listener.Z + sim.Origin.Z:0.0} (eyes {eyes.X:0.0},{eyes.Y:0.0},{eyes.Z:0.0}); origin {sim.Origin.X},{sim.Origin.Y},{sim.Origin.Z}")
                .Append(sim.Origin == wo ? string.Empty : string.Create(CultureInfo.InvariantCulture, $" MISMATCH: scene streamer has {wo.X},{wo.Y},{wo.Z}"));
        }
        if ((renderer.Overlay & SceneOverlay.Sources) == 0 || capi.World.Player?.Entity is not { } player)
        {
            return text.ToString();
        }

        (int X, int Y, int Z) o = world.Status().Origin;
        Vec3d eye = Eyes(player);
        foreach ((SourceDebugInfo s, double distance) in sources
            .Select(s => (s, Distance(s, o, eye)))
            .OrderBy(p => p.Item2)
            .Take(6))
        {
            (float low, float mid, float high) = s.Gain;
            string name = describeVoice(s.Voice) ?? $"voice {s.Voice}";
            text.Append(CultureInfo.InvariantCulture, $"\n  {name} {distance:0.0} m: visible {s.Occlusion:0.00}, ")
                .Append(s.Crossings == 0
                    ? "clear line"
                    : string.Create(CultureInfo.InvariantCulture, $"{s.SolidMetres:0.0} m through {s.Crossings} material(s)"))
                .Append(CultureInfo.InvariantCulture, $" -> {Db(low):0}/{Db(mid):0}/{Db(high):0} dB")
                .Append(s.Escaped ? " (moved out of its block)" : string.Empty);
        }

        return text.ToString();
    }

    private static double Distance(SourceDebugInfo s, (int X, int Y, int Z) o, Vec3d eye)
    {
        double dx = s.Position.X + o.X - eye.X;
        double dy = s.Position.Y + o.Y - eye.Y;
        double dz = s.Position.Z + o.Z - eye.Z;
        return Math.Sqrt((dx * dx) + (dy * dy) + (dz * dz));
    }

    /// <summary>
    /// Where the player hears from: the head. (EntityPlayer.CameraPos is the origin the game
    /// renders relative to, at the feet; lines drawn "from the camera" with it ended there.)
    /// </summary>
    private static Vec3d Eyes(EntityPlayer player) => player.Pos.XYZ.Add(player.LocalEyePos);

    private static double Db(float gain) => 20.0 * Math.Log10(Math.Max(gain, 1e-6f));

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
        text.Append('\n').Append(SourcesText());
        text.Append('\n').Append(ReflectionsText(detailed: false));
        text.Append('\n').Append(PathingText());

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

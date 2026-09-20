using System.Diagnostics;
using System.Globalization;
using System.Text;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Takeover;

namespace VintageStorySteamAudio.Diagnostics;

/// <summary>A thread's, or a group of threads', share of one core over the window.</summary>
/// <param name="Name">The engine's name, or the module.</param>
/// <param name="Kind">Whose.</param>
/// <param name="Threads">How many threads the row sums.</param>
/// <param name="CorePercent">CPU time over wall time, in percent of one core.</param>
public readonly record struct ThreadShare(string Name, ThreadKind Kind, int Threads, double CorePercent);

/// <summary>
/// The <c>.steamaudio perf</c> report (Phase 8): what the mod costs, over a window since the
/// last reset. The main thread from <see cref="PerfMonitor"/>; every thread's share of a core
/// from two readings of the engine's thread stats; the process's CPU and memory; the engine's
/// own telemetry. Vanilla has no such report, so the process-level figures are what a run with
/// the mod disabled is compared against (scripts/perf-sample.ps1 samples them from outside).
/// </summary>
public sealed class PerfReporter
{
    private readonly PerfMonitor monitor;
    private readonly AudioEngine engine;
    private readonly Func<AudioSession?> session;
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private IReadOnlyList<ThreadStats> startThreads = [];
    private TimeSpan startProcessCpu;
    private long startAllocated;
    private int[] startCollections = new int[3];
    private ulong startOverloads;
    private ulong startUnderruns;
    private long startBakes;
    private long startCancelled;
    private long startChunksBuilt;

    public PerfReporter(PerfMonitor monitor, AudioEngine engine, Func<AudioSession?> session)
    {
        this.monitor = monitor;
        this.engine = engine;
        this.session = session;
        Reset();
    }

    /// <summary>Starts a new window now.</summary>
    public void Reset()
    {
        monitor.Reset();
        clock.Restart();
        startThreads = engine.GetThreadStats();
        using Process process = Process.GetCurrentProcess();
        startProcessCpu = process.TotalProcessorTime;
        startAllocated = GC.GetTotalAllocatedBytes(precise: false);
        startCollections = [GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2)];
        EngineStats stats = engine.GetStats();
        startOverloads = stats.Overloads;
        startUnderruns = stats.StreamUnderruns;
        startBakes = TryGet(engine.GetPathingStats)?.Bakes ?? 0;
        startCancelled = TryGet(engine.GetPathingStats)?.CancelledBakes ?? 0;
        startChunksBuilt = TryGet(engine.GetSceneStats)?.ChunksBuilt ?? 0;
    }

    /// <summary>".steamaudio perf [reset]".</summary>
    public string Command(string? action)
    {
        if (string.Equals(action, "reset", StringComparison.OrdinalIgnoreCase))
        {
            Reset();
            return "Perf window reset. Play for a while, then .steamaudio perf";
        }

        return action is null ? Report() : "Usage: .steamaudio perf [reset]";
    }

    /// <summary>
    /// Each thread's CPU over the window as a share of one core: the engine's threads one row
    /// each, Steam Audio's workers one row, the rest one row per module, largest first.
    /// </summary>
    public static IReadOnlyList<ThreadShare> ThreadShares(IReadOnlyList<ThreadStats> start, IReadOnlyList<ThreadStats> end, double wallMs)
    {
        ArgumentNullException.ThrowIfNull(start);
        ArgumentNullException.ThrowIfNull(end);
        if (wallMs <= 0.0)
        {
            return [];
        }

        var before = new Dictionary<uint, double>();
        foreach (ThreadStats t in start)
        {
            before[t.ThreadId] = t.CpuMs;
        }

        var rows = new Dictionary<(string Name, ThreadKind Kind), (int Threads, double CpuMs)>();
        foreach (ThreadStats t in end)
        {
            // A thread that started inside the window counts from its start.
            double cpu = t.CpuMs - before.GetValueOrDefault(t.ThreadId, 0.0);
            if (cpu < 0.0)
            {
                cpu = 0.0;  // an id reused by a new thread
            }

            (string Name, ThreadKind Kind) key = t.Kind == ThreadKind.SteamAudio ? ("steam audio workers", t.Kind) : (t.Name, t.Kind);
            (int Threads, double CpuMs) row = rows.GetValueOrDefault(key);
            rows[key] = (row.Threads + 1, row.CpuMs + cpu);
        }

        return rows
            .Select(r => new ThreadShare(r.Key.Name, r.Key.Kind, r.Value.Threads, 100.0 * r.Value.CpuMs / wallMs))
            .OrderBy(r => r.Kind)
            .ThenByDescending(r => r.CorePercent)
            .ToList();
    }

    public string Report()
    {
        double wallMs = clock.Elapsed.TotalMilliseconds;
        PerfSnapshot snap = monitor.Snapshot();
        IReadOnlyList<ThreadShare> shares = ThreadShares(startThreads, engine.GetThreadStats(), wallMs);
        using Process process = Process.GetCurrentProcess();
        double processCores = (process.TotalProcessorTime - startProcessCpu).TotalMilliseconds / wallMs;
        EngineStats stats = engine.GetStats();
        SceneStats? scene = TryGet(engine.GetSceneStats);
        SimulationStats? direct = TryGet(engine.GetSimulationStats);
        ReflectionStats? reflections = TryGet(engine.GetReflectionStats);
        PathingStats? pathing = TryGet(engine.GetPathingStats);
        AudioSession? audio = session();

        var text = new StringBuilder();
        text.Append(audio is null
            ? "Audio: vanilla OpenAL (this mod is not playing it) - the baseline run\n"
            : "Audio: this mod\n");
        text.Append(F($"Perf window: {snap.WindowSeconds:0.0} s, {snap.Frames} frames"));
        if (snap.Frames > 0)
        {
            text.Append(F($" ({snap.Fps:0.0} fps; frame {snap.FrameMedianMs:0.0} ms median, {snap.FrameP99Ms:0.0} ms p99, {snap.FrameMaxMs:0.0} ms worst)"));
        }

        text.Append('\n');
        text.Append(F($"Main thread, ours: {snap.MainThreadMsPerFrame:0.000} ms per frame (budget 0.5), {snap.Sections.Sum(s => s.AllocatedBytes) / 1048576.0:0.0} MB allocated\n"));
        foreach (PerfSectionTotals s in snap.Sections.Where(s => s.Calls > 0).OrderByDescending(s => s.TotalMs))
        {
            double perFrame = snap.Frames > 0 ? s.TotalMs / snap.Frames : 0.0;
            text.Append(F($"  {Label(s.Section),-16} {perFrame,7:0.000} ms/frame  {s.Calls,7} calls, {s.SlowCalls,5} over {PerfMonitor.SlowCallMs:0} ms, worst {s.MaxMs,6:0.00} ms, {s.AllocatedBytes / 1024.0,8:0} KB\n"));
        }

        text.Append("Threads, share of one core:\n");
        AppendShares(text, shares, ThreadKind.Engine, "engine");
        AppendShares(text, shares, ThreadKind.SteamAudio, "Steam Audio");
        AppendShares(text, shares, ThreadKind.Other, "game and runtime");
        text.Append(F($"  process: {100.0 * processCores:0} % of one core ({processCores:0.00} cores of {Environment.ProcessorCount})\n"));

        text.Append(F($"Engine: render avg {stats.RenderTimeAvgUs:0} us, max {stats.RenderTimeMaxUs:0} us of the {stats.BlockPeriodUs / 1000.0:0.00} ms block ({stats.RenderTimeMaxUs / Math.Max(1.0, stats.BlockPeriodUs):P0} worst); "));
        text.Append(F($"{stats.Overloads - startOverloads} overloads, {stats.StreamUnderruns - startUnderruns} stream underruns; {stats.ActiveVoices} voices ({stats.RealVoices} with effects, {stats.VirtualVoices} virtual)\n"));
        if (direct is not null || reflections is not null || pathing is not null)
        {
            text.Append("Simulation:");
            if (direct is not null)
            {
                text.Append(F($" direct {direct.LastTickMs:0.00} ms/tick at {direct.RateHz} Hz, {direct.Sources} sources;"));
            }

            if (reflections is { Enabled: true })
            {
                text.Append(F($" reflections {reflections.LastTickMs:0.0} ms/tick (worst {reflections.MaxTickMs:0.0}) at {reflections.RateHz} Hz, {reflections.LiveSlots} places, {reflections.Threads} threads;"));
            }

            if (pathing is { Enabled: true })
            {
                long bakes = pathing.Bakes - startBakes;
                double bakeShare = wallMs > 0.0 ? 100.0 * bakes * pathing.LastBakeMs / wallMs : 0.0;
                text.Append(F($" pathing {pathing.LastTickMs:0.00} ms/tick, {bakes} bakes this window ({bakeShare:0} % of it baking, {pathing.CancelledBakes - startCancelled} abandoned), {pathing.LastBakeMs:0} ms each (worst {pathing.MaxBakeMs:0}), {pathing.Probes} probes {pathing.ProbeSpacing:0.0} m apart;"));
            }

            text.Length--;
            text.Append('\n');
        }

        long allocated = GC.GetTotalAllocatedBytes(precise: false) - startAllocated;
        text.Append(F($"Memory: working set {process.WorkingSet64 / 1048576.0:0} MB, private {process.PrivateMemorySize64 / 1048576.0:0} MB; "));
        text.Append(F($"GC heap {GC.GetTotalMemory(false) / 1048576.0:0} MB, {allocated / 1048576.0 / Math.Max(0.001, wallMs / 1000.0):0.0} MB/s allocated by all, collections gen0 {GC.CollectionCount(0) - startCollections[0]} gen1 {GC.CollectionCount(1) - startCollections[1]} gen2 {GC.CollectionCount(2) - startCollections[2]}"));
        if (scene is not null)
        {
            text.Append(F($"; scene {scene.Chunks} chunks ({scene.ChunksBuilt - startChunksBuilt} meshed this window, worst {scene.MaxBuildMs:0} ms), {scene.Triangles / 1000.0:0} k triangles, {scene.MemoryBytes / 1048576.0:0.0} MB"));
        }

        if (audio is not null)
        {
            text.Append(F($"; assets {audio.Assets.MemoryBytes / 1048576.0:0.0} MB"));
        }

        return text.ToString();
    }

    private static void AppendShares(StringBuilder text, IReadOnlyList<ThreadShare> shares, ThreadKind kind, string heading)
    {
        List<ThreadShare> rows = shares.Where(s => s.Kind == kind).ToList();
        if (rows.Count == 0)
        {
            return;
        }

        double total = rows.Sum(r => r.CorePercent);
        text.Append(F($"  {heading} {total:0.0} %: "));
        // The small fry of the game's threads in one figure.
        List<ThreadShare> shown = kind == ThreadKind.Other ? rows.Where(r => r.CorePercent >= 0.5).ToList() : rows;
        text.Append(string.Join(", ", shown.Select(r => F($"{r.Name}{(r.Threads > 1 ? " x" + r.Threads.ToString(CultureInfo.InvariantCulture) : "")} {r.CorePercent:0.0}"))));
        double rest = total - shown.Sum(r => r.CorePercent);
        if (shown.Count < rows.Count)
        {
            text.Append(F($", {rows.Count - shown.Count} others {rest:0.0}"));
        }

        text.Append('\n');
    }

    private static string Label(PerfSection section) => section switch
    {
        PerfSection.Listener => "listener",
        PerfSection.EntityTracking => "entity tracking",
        PerfSection.Pump => "events, settings",
        PerfSection.SoundCreate => "sound creation",
        PerfSection.SoundApi => "sound API",
        PerfSection.SceneTick => "scene tick",
        PerfSection.ChunkRead => "  chunk reads",
        PerfSection.Playback => "test playback",
        PerfSection.Hud => "debug HUD",
        PerfSection.Overlay => "debug overlay",
        _ => section.ToString(),
    };

    private static T? TryGet<T>(Func<T> get)
        where T : class
    {
        try
        {
            return get();
        }
        catch (NativeException)
        {
            return null;
        }
    }

    private static string F(FormattableString s) => s.ToString(CultureInfo.InvariantCulture);
}

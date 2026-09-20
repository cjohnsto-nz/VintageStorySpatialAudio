using System.Diagnostics;

namespace VintageStorySteamAudio.Diagnostics;

/// <summary>The mod's work on the game's main thread, by what it is for.</summary>
public enum PerfSection
{
    /// <summary>Handing the game's listener pose to the engine, once per frame.</summary>
    Listener,

    /// <summary>Following entities with their sounds, once per frame.</summary>
    EntityTracking,

    /// <summary>Draining the engine's events (fades done, sounds ended) and polling settings, once per frame.</summary>
    Pump,

    /// <summary>Creating a game sound: reading and decoding its asset when the game has not, and the voice.</summary>
    SoundCreate,

    /// <summary>The ILoadedSound calls: start, stop, position, volume, pitch, fades.</summary>
    SoundApi,

    /// <summary>The world scene's tick: chunks changed, chunks to read, the origin (every 50 ms).</summary>
    SceneTick,

    /// <summary>Of that, copying a chunk's blocks into a snapshot for the engine.</summary>
    ChunkRead,

    /// <summary>The test playback tick (only without the takeover).</summary>
    Playback,

    /// <summary>The debug HUD's text (every 100 ms while shown).</summary>
    Hud,

    /// <summary>Drawing the debug overlay (every frame while shown).</summary>
    Overlay,
}

/// <summary>One section's totals over the window.</summary>
/// <param name="Section">Which.</param>
/// <param name="Calls">Scopes entered (outermost only).</param>
/// <param name="SlowCalls">Of those, the ones long enough to cost a frame (see <see cref="PerfMonitor.SlowCallMs"/>).</param>
/// <param name="TotalMs">Time inside, summed.</param>
/// <param name="MaxMs">The longest single scope.</param>
/// <param name="AllocatedBytes">Managed memory allocated inside, summed.</param>
public readonly record struct PerfSectionTotals(PerfSection Section, long Calls, long SlowCalls, double TotalMs, double MaxMs, long AllocatedBytes);

/// <summary>What the monitor saw since its last reset.</summary>
/// <param name="WindowSeconds">Wall time since the reset.</param>
/// <param name="Frames">Frames seen (the game's listener updates).</param>
/// <param name="FrameMedianMs">The median frame time, from the histogram (0 without frames).</param>
/// <param name="FrameP99Ms">The 99th-percentile frame time.</param>
/// <param name="FrameMaxMs">The longest frame.</param>
/// <param name="Sections">Every section's totals, in enum order.</param>
public sealed record PerfSnapshot(
    double WindowSeconds,
    long Frames,
    double FrameMedianMs,
    double FrameP99Ms,
    double FrameMaxMs,
    IReadOnlyList<PerfSectionTotals> Sections)
{
    /// <summary>The mod's main-thread time per frame, all sections, in ms (0 without frames).</summary>
    public double MainThreadMsPerFrame => Frames == 0 ? 0.0 : Sections.Sum(s => s.TotalMs) / Frames;

    /// <summary>Frames per second over the window (0 without a window).</summary>
    public double Fps => WindowSeconds <= 0.0 ? 0.0 : Frames / WindowSeconds;
}

/// <summary>
/// Times the mod's work on the game's main thread by section, with the managed memory it
/// allocates, and the frames between the game's listener updates (Phase 8: what the mod costs
/// the frame, against PLAN §9's 0.5 ms). Hooks take a scope:
/// <c>using (PerfMonitor.Instance.Measure(PerfSection.SceneTick)) { ... }</c>. Cheap enough to
/// leave on: a Stopwatch read and an allocation counter read per scope. Nested scopes of one
/// section count once. Only the main thread is recorded (a fade's completion runs on the pool):
/// the first thread to call <see cref="Frame"/> or <see cref="AttachThread"/> is it.
/// </summary>
public sealed class PerfMonitor
{
    /// <summary>
    /// A call at least this long is counted apart: on a 60 fps frame of 16.7 ms, one of these
    /// is a visible part of the frame, and the average hides them.
    /// </summary>
    public const double SlowCallMs = 2.0;

    private const double FrameBinMs = 0.25;
    private const int FrameBins = 800;  // to 200 ms

    /// <summary>Process-wide, like the threads it measures.</summary>
    public static PerfMonitor Instance { get; } = new();

    // A property, not a field: a static field declared after Instance would still be 0 while
    // Instance is constructed.
    private static int SectionCount => Enum.GetValues<PerfSection>().Length;

    private readonly long[] ticks = new long[SectionCount];
    private readonly long[] calls = new long[SectionCount];
    private readonly long[] slowCalls = new long[SectionCount];
    private readonly long[] maxTicks = new long[SectionCount];
    private readonly long[] bytes = new long[SectionCount];
    private readonly int[] depth = new int[SectionCount];
    private readonly long[] frameBins = new long[FrameBins];
    private long frames;
    private long frameMaxTicks;
    private long lastFrameTicks = -1;
    private long windowStartTicks = Stopwatch.GetTimestamp();
    private int threadId = -1;

    /// <summary>Makes the calling thread the one recorded (the game's main thread).</summary>
    public void AttachThread() => threadId = Environment.CurrentManagedThreadId;

    /// <summary>A frame boundary: the game's per-frame listener update.</summary>
    public void Frame()
    {
        if (threadId < 0)
        {
            AttachThread();
        }

        long now = Stopwatch.GetTimestamp();
        if (lastFrameTicks >= 0)
        {
            RecordFrame(now - lastFrameTicks);
        }

        lastFrameTicks = now;
    }

    /// <summary>Starts a scope of <paramref name="section"/>; dispose it to record.</summary>
    public Scope Measure(PerfSection section) => new(this, section);

    /// <summary>Forgets everything and starts a new window now.</summary>
    public void Reset()
    {
        Array.Clear(ticks);
        Array.Clear(calls);
        Array.Clear(slowCalls);
        Array.Clear(maxTicks);
        Array.Clear(bytes);
        Array.Clear(frameBins);
        frames = 0;
        frameMaxTicks = 0;
        lastFrameTicks = -1;
        windowStartTicks = Stopwatch.GetTimestamp();
    }

    public PerfSnapshot Snapshot()
    {
        var sections = new PerfSectionTotals[SectionCount];
        for (int i = 0; i < SectionCount; i++)
        {
            sections[i] = new PerfSectionTotals((PerfSection)i, calls[i], slowCalls[i], Ms(ticks[i]), Ms(maxTicks[i]), bytes[i]);
        }

        return new PerfSnapshot(
            Ms(Stopwatch.GetTimestamp() - windowStartTicks) / 1000.0,
            frames,
            Percentile(0.5),
            Percentile(0.99),
            Ms(frameMaxTicks),
            sections);
    }

    internal void RecordFrame(long elapsedTicks)
    {
        frames++;
        frameMaxTicks = Math.Max(frameMaxTicks, elapsedTicks);
        int bin = (int)Math.Min(FrameBins - 1, Math.Max(0.0, Ms(elapsedTicks) / FrameBinMs));
        frameBins[bin]++;
    }

    internal void Record(PerfSection section, long elapsedTicks, long allocated)
    {
        int i = (int)section;
        ticks[i] += elapsedTicks;
        calls[i]++;
        if (Ms(elapsedTicks) >= SlowCallMs)
        {
            slowCalls[i]++;
        }

        maxTicks[i] = Math.Max(maxTicks[i], elapsedTicks);
        bytes[i] += Math.Max(0, allocated);
    }

    private double Percentile(double fraction)
    {
        if (frames == 0)
        {
            return 0.0;
        }

        long wanted = (long)Math.Ceiling(frames * fraction);
        long seen = 0;
        for (int i = 0; i < FrameBins; i++)
        {
            seen += frameBins[i];
            if (seen >= wanted)
            {
                return (i + 0.5) * FrameBinMs;
            }
        }

        return FrameBins * FrameBinMs;
    }

    private static double Ms(long stopwatchTicks) => stopwatchTicks * 1000.0 / Stopwatch.Frequency;

    /// <summary>A section entered; disposing records it (outermost of its section only).</summary>
    public readonly struct Scope : IDisposable
    {
        private readonly PerfMonitor? monitor;
        private readonly PerfSection section;
        private readonly long startTicks;
        private readonly long startBytes;

        internal Scope(PerfMonitor monitor, PerfSection section)
        {
            if (monitor.threadId >= 0 && monitor.threadId != Environment.CurrentManagedThreadId)
            {
                this.monitor = null;  // another thread: not measured
                return;
            }

            this.monitor = monitor;
            this.section = section;
            monitor.depth[(int)section]++;
            startTicks = Stopwatch.GetTimestamp();
            startBytes = GC.GetAllocatedBytesForCurrentThread();
        }

        public void Dispose()
        {
            if (monitor is null)
            {
                return;
            }

            int i = (int)section;
            if (--monitor.depth[i] == 0)
            {
                monitor.Record(section, Stopwatch.GetTimestamp() - startTicks, GC.GetAllocatedBytesForCurrentThread() - startBytes);
            }
        }
    }
}

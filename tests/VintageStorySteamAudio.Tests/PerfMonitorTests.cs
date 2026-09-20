using System.Diagnostics;
using VintageStorySteamAudio.Diagnostics;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Tests;

/// <summary>The main-thread profiler (Phase 8): sections, nesting, frames, and the thread shares.</summary>
public sealed class PerfMonitorTests
{
    [Fact]
    public void Sections_accumulate_calls_time_and_the_worst_call()
    {
        var perf = new PerfMonitor();
        perf.AttachThread();
        for (int i = 0; i < 3; i++)
        {
            using (perf.Measure(PerfSection.SceneTick))
            {
                Spin(i == 1 ? 4.0 : 0.5);
            }
        }

        PerfSectionTotals tick = perf.Snapshot().Sections.Single(s => s.Section == PerfSection.SceneTick);
        Assert.Equal(3, tick.Calls);
        Assert.InRange(tick.TotalMs, 4.5, 60.0);
        Assert.InRange(tick.MaxMs, 3.5, 60.0);
        Assert.True(tick.MaxMs <= tick.TotalMs);
        Assert.Equal(0, perf.Snapshot().Sections.Single(s => s.Section == PerfSection.Hud).Calls);
    }

    [Fact]
    public void Calls_long_enough_to_cost_a_frame_are_counted_apart()
    {
        var perf = new PerfMonitor();
        perf.AttachThread();
        using (perf.Measure(PerfSection.ChunkRead))
        {
            Spin(PerfMonitor.SlowCallMs * 2.0);
        }

        for (int i = 0; i < 5; i++)
        {
            using (perf.Measure(PerfSection.ChunkRead))
            {
                Spin(0.1);
            }
        }

        PerfSectionTotals read = perf.Snapshot().Sections.Single(s => s.Section == PerfSection.ChunkRead);
        Assert.Equal(6, read.Calls);
        Assert.Equal(1, read.SlowCalls);
    }

    [Fact]
    public void A_nested_scope_of_the_same_section_counts_once_and_allocations_are_attributed()
    {
        var perf = new PerfMonitor();
        perf.AttachThread();
        using (perf.Measure(PerfSection.SoundApi))
        {
            using (perf.Measure(PerfSection.SoundApi))
            {
                _ = new byte[64 * 1024];
            }
        }

        PerfSectionTotals api = perf.Snapshot().Sections.Single(s => s.Section == PerfSection.SoundApi);
        Assert.Equal(1, api.Calls);
        Assert.InRange(api.AllocatedBytes, 64 * 1024, 256 * 1024);
    }

    [Fact]
    public void The_shared_instance_records_every_section()
    {
        // The singleton is what the hooks use; it must be built whole (static initialisation order).
        foreach (PerfSection section in Enum.GetValues<PerfSection>())
        {
            using (PerfMonitor.Instance.Measure(section))
            {
            }
        }

        Assert.All(PerfMonitor.Instance.Snapshot().Sections, s => Assert.True(s.Calls >= 1));
    }

    [Fact]
    public void Frame_boundaries_come_from_the_gaps_between_calls()
    {
        // The frame probe calls Frame() once per frame; the first call only opens the window.
        var perf = new PerfMonitor();
        perf.Frame();
        Assert.Equal(0, perf.Snapshot().Frames);
        perf.Frame();
        perf.Frame();
        Assert.Equal(2, perf.Snapshot().Frames);
    }

    [Fact]
    public void Another_thread_is_not_recorded()
    {
        var perf = new PerfMonitor();
        perf.AttachThread();
        var worker = new Thread(() =>
        {
            using (perf.Measure(PerfSection.SoundApi))
            {
                Spin(0.2);
            }
        });
        worker.Start();
        worker.Join();
        Assert.Equal(0, perf.Snapshot().Sections.Single(s => s.Section == PerfSection.SoundApi).Calls);
    }

    [Fact]
    public void Frames_give_median_p99_and_worst_from_the_histogram()
    {
        var perf = new PerfMonitor();
        // 99 frames of 16 ms and one of 50 ms.
        for (int i = 0; i < 99; i++)
        {
            perf.RecordFrame(Ticks(16.0));
        }

        perf.RecordFrame(Ticks(50.0));
        PerfSnapshot snap = perf.Snapshot();
        Assert.Equal(100, snap.Frames);
        Assert.InRange(snap.FrameMedianMs, 15.5, 16.5);
        Assert.InRange(snap.FrameP99Ms, 15.5, 16.5);  // the 99th of 100 is still a 16 ms frame
        Assert.InRange(snap.FrameMaxMs, 49.9, 50.1);
        perf.Reset();
        Assert.Equal(0, perf.Snapshot().Frames);
    }

    [Fact]
    public void Thread_shares_are_per_engine_thread_one_row_for_Steam_Audio_and_by_module_for_the_rest()
    {
        ThreadStats[] start =
        [
            new("render (device callback)", ThreadKind.Engine, 1, 100.0),
            new("direct simulation", ThreadKind.Engine, 2, 50.0),
            new("steam audio worker", ThreadKind.SteamAudio, 3, 10.0),
            new("steam audio worker", ThreadKind.SteamAudio, 4, 10.0),
            new("Vintagestory.exe", ThreadKind.Other, 5, 1000.0),
        ];
        ThreadStats[] end =
        [
            new("render (device callback)", ThreadKind.Engine, 1, 200.0),   // +100 of 1000 ms: 10 %
            new("direct simulation", ThreadKind.Engine, 2, 80.0),            // 3 %
            new("steam audio worker", ThreadKind.SteamAudio, 3, 110.0),      // +100
            new("steam audio worker", ThreadKind.SteamAudio, 4, 160.0),      // +150: 25 % together
            new("Vintagestory.exe", ThreadKind.Other, 5, 1900.0),            // 90 %
            new("coreclr.dll", ThreadKind.Other, 6, 40.0),                   // new this window: 4 %
        ];
        IReadOnlyList<ThreadShare> shares = PerfReporter.ThreadShares(start, end, 1000.0);
        Assert.Equal(5, shares.Count);
        Assert.Equal(new ThreadShare("render (device callback)", ThreadKind.Engine, 1, 10.0), shares[0]);
        Assert.Equal(new ThreadShare("direct simulation", ThreadKind.Engine, 1, 3.0), shares[1]);
        Assert.Equal(new ThreadShare("steam audio workers", ThreadKind.SteamAudio, 2, 25.0), shares[2]);
        Assert.Equal(new ThreadShare("Vintagestory.exe", ThreadKind.Other, 1, 90.0), shares[3]);
        Assert.Equal(new ThreadShare("coreclr.dll", ThreadKind.Other, 1, 4.0), shares[4]);
        Assert.Empty(PerfReporter.ThreadShares(start, end, 0.0));
    }

    [Fact]
    public void The_profiler_costs_little_enough_to_leave_on_in_a_release()
    {
        // It measures every frame of a published build, so its own cost has to be lost in the
        // noise. The busiest section in play is the sound API at a few hundred calls a second.
        var perf = new PerfMonitor();
        perf.AttachThread();
        for (int i = 0; i < 10_000; i++)
        {
            using (perf.Measure(PerfSection.SoundApi))
            {
            }
        }

        const int Calls = 500_000;
        long start = Stopwatch.GetTimestamp();
        for (int i = 0; i < Calls; i++)
        {
            using (perf.Measure(PerfSection.SoundApi))
            {
            }
        }

        double nanosecondsPerCall = (Stopwatch.GetTimestamp() - start) * 1e9 / Stopwatch.Frequency / Calls;
        Assert.InRange(nanosecondsPerCall, 0.0, 2000.0);
        // At 1000 calls a second -- far more than any frame makes -- that is under 0.2 % of one core.
        Assert.True(nanosecondsPerCall * 1000 < 0.002 * 1e9, $"{nanosecondsPerCall:0} ns per scope");
    }

    [Fact]
    public void The_cost_breakdown_ranks_the_parts_and_leaves_the_game_out()
    {
        ThreadShare[] shares =
        [
            new("steam audio workers", ThreadKind.SteamAudio, 14, 82.1),
            new("reflection simulation", ThreadKind.Engine, 1, 15.4),
            new("render (spatial audio)", ThreadKind.Engine, 1, 11.0),
            new("path baker", ThreadKind.Engine, 1, 2.5),
            new("pathing simulation", ThreadKind.Engine, 1, 0.01),   // below the floor
            new("Vintagestory.exe", ThreadKind.Other, 1, 42.2),      // the game's, not ours
        ];
        var perf = new PerfMonitor();
        perf.AttachThread();
        using (perf.Measure(PerfSection.SceneTick))
        {
            Spin(5.0);
        }

        IReadOnlyList<CostRow> costs = PerfReporter.Costs(shares, perf.Snapshot(), null, null);
        Assert.True(costs.Zip(costs.Skip(1)).All(p => p.First.CorePercent >= p.Second.CorePercent), "largest first");
        Assert.Equal(82.1, costs.Single(c => c.Name == "ray tracing (Steam Audio)").CorePercent);
        Assert.Equal(15.4, costs.Single(c => c.Name == "reflections: the simulation").CorePercent);
        Assert.Equal(11.0, costs.Single(c => c.Name == "rendering the sound").CorePercent);
        Assert.Equal(2.5, costs.Single(c => c.Name == "pathing: the bake").CorePercent);
        Assert.Contains(costs, c => c.Name == "the game's own frame");       // our main-thread work
        Assert.DoesNotContain(costs, c => c.Name.Contains("Vintagestory"));  // never the game's own
        Assert.DoesNotContain(costs, c => c.Name == "pathing: the simulation");  // too small to list
    }

    private static void Spin(double ms)
    {
        long until = Stopwatch.GetTimestamp() + Ticks(ms);
        while (Stopwatch.GetTimestamp() < until)
        {
        }
    }

    private static long Ticks(double ms) => (long)(ms / 1000.0 * Stopwatch.Frequency);
}

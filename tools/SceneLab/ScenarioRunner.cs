using System.Diagnostics;
using VintageStorySteamAudio.Native;

namespace SceneLab;

public sealed record ChannelMetrics(double PeakDbfs, double RmsDbfs);

public sealed record RenderMetrics(
    int Blocks, int BlockFrames, double BlockPeriodUs, double AvgUs, double P99Us, double MaxUs, double P99Load, ulong Overloads);

public sealed record EventRecord(double Time, EngineEventType Type, ulong Voice, ulong Token, bool FadeCancelled);

public sealed record ScenarioMetrics(
    string Scenario,
    int SampleRate,
    int Channels,
    long Frames,
    int Voices,
    IReadOnlyList<ChannelMetrics> ChannelLevels,
    RenderMetrics Render,
    ulong StreamUnderruns,
    float LimiterPeakReductionDb,
    IReadOnlyList<EventRecord> Events,
    IReadOnlyList<string> Failures);

public sealed record ScenarioResult(float[] Samples, ScenarioMetrics Metrics);

/// <summary>Renders a <see cref="Scenario"/> offline. Creates (and disposes) the process's engine.</summary>
public static class ScenarioRunner
{
    public static ScenarioResult Run(Scenario scenario, string baseDirectory, IEngineLog? log = null)
    {
        ArgumentNullException.ThrowIfNull(scenario);

        using AudioEngine engine = AudioEngine.Create(
            new EngineOptions
            {
                RayTracer = RayTracer.Steam, // Phase 1 renders no geometry; skip Embree start-up
                SampleRate = scenario.SampleRate,
                BlockFrames = scenario.Engine.BlockFrames,
                MaxVoices = scenario.Engine.MaxVoices,
                ResamplerQuality = scenario.Engine.ResamplerQuality,
                StreamThresholdMs = scenario.Engine.StreamThresholdMs,
                MaxRealVoices = scenario.Engine.MaxRealVoices,
                MaxBinauralVoices = scenario.Engine.MaxBinauralVoices,
            },
            log);
        engine.SetRenderMode(scenario.RenderMode);
        if (scenario.Listener is ListenerSpec l)
        {
            engine.SetListener(l.Position[0], l.Position[1], l.Position[2], l.Forward[0], l.Forward[1], l.Forward[2], l.Up[0], l.Up[1], l.Up[2]);
        }
        engine.OpenOffline(scenario.SampleRate, scenario.Channels);
        engine.SetMasterGain(scenario.MasterGain);
        foreach ((string bus, float gain) in scenario.Buses)
        {
            engine.SetBusGain(Enum.Parse<AudioBus>(bus, ignoreCase: true), gain);
        }

        var assets = new Dictionary<string, AudioAsset>(StringComparer.Ordinal);
        var voices = new List<Voice>();
        try
        {
            foreach ((string name, AssetSpec spec) in scenario.Assets)
            {
                assets[name] = CreateAsset(engine, name, spec, baseDirectory);
            }

            var schedule = new List<(double Time, int Order, Action Apply)>();
            var orbits = new List<(Voice Voice, OrbitSpec Orbit, double Phase)>();
            foreach (VoiceSpec spec in scenario.Voices)
            {
                if (!assets.TryGetValue(spec.Asset, out AudioAsset? asset))
                {
                    throw new InvalidDataException($"voice refers to unknown asset '{spec.Asset}'");
                }

                for (int i = 0; i < spec.Count; i++)
                {
                    float spread = spec.Count > 1 ? spec.PitchSpread * ((2f * i / (spec.Count - 1)) - 1f) : 0f;
                    SpatialMode mode = spec.Orbit is null ? spec.Spatial : SpatialMode.World;
                    var placement = new VoicePlacement(mode, spec.Position[0], spec.Position[1], spec.Position[2], spec.MinDistance);
                    Voice voice = engine.CreateVoice(asset, spec.Bus, spec.Gain, spec.Pitch + spread, spec.Loop, placement);
                    voices.Add(voice);
                    if (spec.Orbit is OrbitSpec orbit)
                    {
                        orbits.Add((voice, orbit, spec.Count > 1 ? (double)i / spec.Count : 0));
                    }
                    double offset = i * spec.StartStagger;
                    if (spec.Start is double start)
                    {
                        schedule.Add((start + offset, schedule.Count, voice.Start));
                    }

                    foreach (VoiceAction action in spec.Actions)
                    {
                        schedule.Add((action.At + offset, schedule.Count, () => Apply(voice, action, mode)));
                    }
                }
            }

            schedule.Sort((a, b) => a.Time != b.Time ? a.Time.CompareTo(b.Time) : a.Order.CompareTo(b.Order));
            return Render(engine, scenario, schedule, orbits, voices.Count);
        }
        finally
        {
            foreach (Voice voice in voices)
            {
                voice.Dispose();
            }

            foreach (AudioAsset asset in assets.Values)
            {
                asset.Dispose();
            }
        }
    }

    private static ScenarioResult Render(
        AudioEngine engine,
        Scenario scenario,
        List<(double Time, int Order, Action Apply)> schedule,
        List<(Voice Voice, OrbitSpec Orbit, double Phase)> orbits,
        int voiceCount)
    {
        int channels = scenario.Channels;
        int rate = scenario.SampleRate;
        int blockFrames = engine.GetStats().BlockFrames;
        long totalFrames = (long)Math.Round(scenario.DurationSeconds * rate);
        float[] samples = new float[totalFrames * channels];
        var times = new List<double>();
        var events = new List<EventRecord>();
        Span<EngineEvent> polled = new EngineEvent[256];
        int next = 0;

        for (long frame = 0; frame < totalFrames; frame += blockFrames)
        {
            double now = (double)frame / rate;
            // Actions land on the block boundary at or after their time.
            while (next < schedule.Count && schedule[next].Time <= now)
            {
                schedule[next++].Apply();
            }

            foreach ((Voice voice, OrbitSpec orbit, double phase) in orbits)
            {
                double angle = 2.0 * Math.PI * ((now / orbit.PeriodSeconds) + phase);
                voice.SetPosition(
                    SpatialMode.World,
                    orbit.Centre[0] + (orbit.Radius * (float)Math.Sin(angle)),
                    orbit.Centre[1],
                    orbit.Centre[2] - (orbit.Radius * (float)Math.Cos(angle)));
            }

            int frames = (int)Math.Min(blockFrames, totalFrames - frame);
            long start = Stopwatch.GetTimestamp();
            engine.RenderOffline(samples.AsSpan((int)(frame * channels), frames * channels));
            times.Add(Stopwatch.GetElapsedTime(start).TotalMicroseconds);

            int count;
            while ((count = engine.PollEvents(polled)) > 0)
            {
                foreach (EngineEvent e in polled[..count])
                {
                    events.Add(new EventRecord(Math.Round(now, 4), e.Type, e.Voice, e.Token, e.FadeCancelled));
                }
            }
        }

        EngineStats stats = engine.GetStats();
        times.Sort();
        double period = 1e6 * blockFrames / rate;
        double p99 = times.Count == 0 ? 0 : times[Math.Min(times.Count - 1, times.Count * 99 / 100)];
        var render = new RenderMetrics(
            times.Count,
            blockFrames,
            period,
            times.Count == 0 ? 0 : times.Average(),
            p99,
            times.Count == 0 ? 0 : times[^1],
            p99 / period,
            stats.Overloads);

        var levels = new List<ChannelMetrics>();
        for (int c = 0; c < channels; c++)
        {
            double peak = 0;
            double sum = 0;
            for (long i = c; i < samples.Length; i += channels)
            {
                double x = samples[i];
                peak = Math.Max(peak, Math.Abs(x));
                sum += x * x;
            }

            levels.Add(new ChannelMetrics(Db(peak), Db(Math.Sqrt(sum / Math.Max(1, totalFrames)))));
        }

        var metrics = new ScenarioMetrics(
            scenario.Name, rate, channels, totalFrames, voiceCount, levels, render, stats.StreamUnderruns,
            stats.LimiterPeakReductionDb, events, []);
        return new ScenarioResult(samples, metrics with { Failures = Check(scenario.Expect, metrics) });
    }

    private static List<string> Check(Expectations? expect, ScenarioMetrics m)
    {
        var failures = new List<string>();
        if (expect is null)
        {
            return failures;
        }

        if (m.Render.Overloads > expect.MaxOverloads)
        {
            failures.Add($"{m.Render.Overloads} overloads > {expect.MaxOverloads}");
        }

        if (m.StreamUnderruns > expect.MaxUnderruns)
        {
            failures.Add($"{m.StreamUnderruns} stream underruns > {expect.MaxUnderruns}");
        }

        double peak = m.ChannelLevels.Max(c => c.PeakDbfs);
        if (peak > expect.MaxPeakDbfs)
        {
            failures.Add($"peak {peak:0.00} dBFS > {expect.MaxPeakDbfs:0.00}");
        }

        double rms = m.ChannelLevels.Max(c => c.RmsDbfs);
        if (rms < expect.MinRmsDbfs)
        {
            failures.Add($"RMS {rms:0.00} dBFS < {expect.MinRmsDbfs:0.00}");
        }

        if (m.Render.P99Load > expect.MaxP99Load)
        {
            failures.Add($"p99 block load {m.Render.P99Load:P1} > {expect.MaxP99Load:P1}");
        }

        return failures;
    }

    private static void Apply(Voice voice, VoiceAction action, SpatialMode mode)
    {
        if (action.Position is { Count: 3 } p) voice.SetPosition(mode == SpatialMode.None ? SpatialMode.World : mode, p[0], p[1], p[2]);
        if (action.Lowpass is float lowpass) voice.SetLowpass(lowpass);
        if (action.Start == true) voice.Start();
        if (action.Pause == true) voice.Pause();
        if (action.Stop == true) voice.Stop();
        if (action.Gain is float gain) voice.SetGain(gain);
        if (action.Pitch is float pitch) voice.SetPitch(pitch);
        if (action.Loop is bool loop) voice.SetLooping(loop);
        if (action.Seek is double seek) voice.Seek(seek);
        if (action.Fade is FadeSpec fade) voice.FadeTo(fade.To, fade.Seconds, stopWhenDone: fade.Stop);
        if (action.Release == true) voice.Dispose();
    }

    private static AudioAsset CreateAsset(AudioEngine engine, string name, AssetSpec spec, string baseDirectory)
    {
        if (spec.File is string file)
        {
            byte[] bytes = File.ReadAllBytes(Path.Combine(baseDirectory, file));
            return engine.CreateAsset(bytes, name, AssetFormat.Auto, spec.Storage);
        }

        if (spec.Tone is ToneSpec tone)
        {
            return engine.CreatePcmAsset(Generate(tone), tone.Channels, tone.SampleRate, name);
        }

        if (spec.Noise is NoiseSpec noise)
        {
            return engine.CreatePcmAsset(Generate(noise), noise.Channels, noise.SampleRate, name);
        }

        throw new InvalidDataException($"asset '{name}' needs one of file, tone or noise");
    }

    internal static short[] Generate(ToneSpec tone)
    {
        int frames = (int)Math.Round(tone.Seconds * tone.SampleRate);
        short[] pcm = new short[frames * tone.Channels];
        for (int i = 0; i < frames; i++)
        {
            double phase = tone.Frequency * i / tone.SampleRate % 1.0;
            double value = tone.Shape switch
            {
                ToneShape.Square => phase < 0.5 ? 1.0 : -1.0,
                ToneShape.Saw => (2.0 * phase) - 1.0,
                _ => Math.Sin(2.0 * Math.PI * phase),
            };
            short sample = ToPcm(tone.Amplitude * value);
            for (int c = 0; c < tone.Channels; c++)
            {
                pcm[(i * tone.Channels) + c] = sample;
            }
        }

        return pcm;
    }

    internal static short[] Generate(NoiseSpec noise)
    {
        int frames = (int)Math.Round(noise.Seconds * noise.SampleRate);
        short[] pcm = new short[frames * noise.Channels];
#pragma warning disable CA5394 // deterministic test signal, not security relevant
        var random = new Random(noise.Seed);
        for (int i = 0; i < pcm.Length; i++)
        {
            pcm[i] = ToPcm(noise.Amplitude * ((2.0 * random.NextDouble()) - 1.0));
        }
#pragma warning restore CA5394

        return pcm;
    }

    private static short ToPcm(double value) => (short)Math.Clamp(Math.Round(value * 32767.0), -32768, 32767);

    private static double Db(double ratio) => 20.0 * Math.Log10(Math.Max(ratio, 1e-12));
}

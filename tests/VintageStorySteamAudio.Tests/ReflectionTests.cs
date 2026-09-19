using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Debugging;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio.Tests;

/// <summary>The reflections' configuration (Phase 6): presets, overrides and what reaches the engine.</summary>
public sealed class ReflectionPresetTests
{
    [Fact]
    public void Medium_is_the_engines_own_default()
    {
        // vsaudio.h documents these defaults; Medium must not quietly differ from them.
        ReflectionQualitySettings medium = ReflectionPresets.For(ReflectionQuality.Medium);
        Assert.Equal(8, medium.Sources);
        Assert.Equal(4096, medium.Rays);
        Assert.Equal(16, medium.Bounces);
        Assert.Equal(1.0f, medium.DurationSeconds);
        Assert.Equal(2, medium.Order);
        Assert.Equal(10, medium.RateHz);
        Assert.Equal(0.1f, medium.TransitionSeconds);
        Assert.Equal(0, medium.Threads);
        Assert.Equal(ReflectionQuality.Medium, new SteamAudioConfig().ReflectionQuality);
        // Voices' own reflections are off unless asked for: every sound feeds the listener's reverb
        // (the engine's default too).
        Assert.False(new SteamAudioConfig().VoiceReflections);
        Assert.Equal(0, new SteamAudioConfig().ToEngineOptions().ReflectionQuality.Sources);
        Assert.Equal(8, new SteamAudioConfig { VoiceReflections = true }.ToEngineOptions().ReflectionQuality.Sources);
    }

    [Fact]
    public void Presets_grow_with_quality_and_stay_within_the_plans_ranges()
    {
        ReflectionQualitySettings[] presets = [.. Enum.GetValues<ReflectionQuality>().Select(ReflectionPresets.For)];
        for (int i = 1; i < presets.Length; i++)
        {
            Assert.True(presets[i].Sources > presets[i - 1].Sources);
            Assert.True(presets[i].Rays > presets[i - 1].Rays);
            Assert.True(presets[i].Bounces > presets[i - 1].Bounces);
            Assert.True(presets[i].DurationSeconds >= presets[i - 1].DurationSeconds);
        }

        foreach (ReflectionQualitySettings p in presets)
        {
            Assert.InRange(p.Sources, 4, 32);
            Assert.InRange(p.Rays, 2048, 16384);
            Assert.InRange(p.Bounces, 8, 32);
            Assert.InRange(p.DurationSeconds, 1.0f, 2.5f);
            Assert.InRange(p.Order, 1, 2);
            Assert.True(p.TransitionSeconds < p.DurationSeconds);
        }
    }

    [Fact]
    public void Overrides_replace_single_values_and_are_clamped_to_what_the_engine_accepts()
    {
        ReflectionQualitySettings r = ReflectionPresets.Resolve(ReflectionQuality.High, new ReflectionQualitySettings
        {
            Sources = 100,
            Rays = 10,
            Order = 3,
            DurationSeconds = 0.3f,
            TransitionSeconds = 0.5f,
        });
        Assert.Equal(64, r.Sources);
        Assert.Equal(256, r.Rays);
        Assert.Equal(24, r.Bounces);  // High's
        Assert.Equal(3, r.Order);
        Assert.Equal(0.3f, r.DurationSeconds);
        Assert.True(r.TransitionSeconds < r.DurationSeconds);

        var config = new SteamAudioConfig { ReflectionQuality = ReflectionQuality.Low, ReflectionBounces = 12, Reflections = false, ReflectionGain = 9f, VoiceReflections = true };
        EngineOptions options = config.ToEngineOptions();
        Assert.False(options.Reflections);
        Assert.Equal(4, options.ReflectionQuality.Sources);
        Assert.Equal(12, options.ReflectionQuality.Bounces);
        Assert.Equal(4f, config.ReflectionGainClamped());
        Assert.Equal((1f, 0f), new SteamAudioConfig { ReflectionTailGain = -2f }.ReflectionMixClamped());
        Assert.Equal(1f, new SteamAudioConfig { ReflectionGain = float.NaN }.ReflectionGainClamped());
    }

    [Theory]
    [InlineData(0f, "not simulated yet")]
    [InlineData(0.15f, "open or deadened")]
    [InlineData(0.4f, "a small room")]
    [InlineData(1.0f, "a large room")]
    [InlineData(1.5f, "a hall")]
    [InlineData(3.0f, "a cave or cathedral")]
    public void The_hud_names_the_space_from_its_decay_time(float rt60, string expected) =>
        Assert.Equal(expected, SceneDebugTools.DescribeSpace(rt60));

    [Fact]
    public void The_ray_view_fades_from_cyan_to_dark_blue_with_the_energy_left()
    {
        int full = SceneDebugRenderer.EnergyColor(1f);
        int spent = SceneDebugRenderer.EnergyColor(1e-3f);
        Assert.Equal(unchecked((int)0xFF8CFFFF), full);
        Assert.True(((spent >> 8) & 0xFF) < 64);  // little green left
        Assert.True((spent & 0xFF) > ((spent >> 16) & 0xFF));  // bluer than red
    }
}

/// <summary>The reflections through the real engine, from managed code.</summary>
[Collection(NativeEngineGroup.Name)]
public sealed class ReflectionEngineTests
{
    [Fact]
    public void A_closed_room_reverberates_and_the_debug_views_show_it()
    {
        NativeTestEnvironment.RequireNatives();
        MaterialTable table = MaterialTable.Build(WorldSceneTests.ShippedConfig());
        using AudioEngine engine = AudioEngine.Create(new EngineOptions
        {
            RayTracer = RayTracer.Steam,
            DirectSimulation = false,
            ReflectionQuality = ReflectionPresets.For(ReflectionQuality.Low),
        }, null);
        engine.SetSceneMaterials(table.Materials);

        // A stone room, 8 x 4 x 8 inside.
        var snapshot = new ChunkSnapshot();
        ushort stone = table.IdOf("stone");
        for (int y = 1; y <= 6; y++)
        {
            for (int z = 1; z <= 10; z++)
            {
                for (int x = 1; x <= 10; x++)
                {
                    if (x is 1 or 10 || y is 1 or 6 || z is 1 or 10)
                    {
                        snapshot.Materials[(y * 32 + z) * 32 + x] = stone;
                    }
                }
            }
        }

        engine.SetSceneChunk(snapshot);
        Assert.True(engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));
        engine.SetListener(6, 3.7f, 6, 0, 0, -1, 0, 1, 0);
        using AudioAsset tone = engine.CreatePcmAsset(NativeEngineTests.Sine(440, 48000, 48000, 0.3), 1, 48000, "tone");
        using Voice voice = engine.CreateVoice(tone, AudioBus.Sound, 1, 1, looping: true, new VoicePlacement(SpatialMode.World, 8, 3.5f, 5));
        voice.Start();
        engine.RenderOffline(new float[48000 * 2]);

        ReflectionStats stats = engine.GetReflectionStats();
        Assert.True(stats.Enabled);
        Assert.Equal(4, stats.Slots);
        Assert.Equal(1, stats.LiveSlots);
        Assert.True(stats.Ticks >= 4);
        Assert.InRange(stats.ListenerReverbTimes.Mid, 0.2f, 3f);
        Assert.True(stats.OutputDb > -60f);

        IReadOnlyList<ReflectionSourceInfo> sources = engine.GetReflectionSources();
        Assert.Equal(2, sources.Count);
        Assert.Equal(0, sources[0].Slot);
        Assert.Equal(voice.Handle, sources[1].Voice);

        IReadOnlyList<RaySegment> rays = engine.TraceRays((6, 3.7f, 6), 16, 4, 40f);
        Assert.Equal(16 * 5, rays.Count);  // a closed room: no path gets out
        Assert.All(rays, r => Assert.Equal(stone, r.Material));

        engine.SetReflectionGain(0.5f);
        Assert.Equal(0.5f, engine.GetReflectionStats().Gain);
        engine.SetReflectionMix(0f, 0.7f);
        Assert.Equal(0f, engine.ReflectionEarlyGain);
        Assert.Equal(0.7f, engine.ReflectionTailGain);
        Assert.Throws<NativeException>(() => engine.SetReflectionMix(5f, 1f));
        Assert.Throws<NativeException>(() => engine.SetReflectionGain(-1f));
    }

    [Fact]
    public void Switched_off_there_are_no_reflections()
    {
        NativeTestEnvironment.RequireNatives();
        using AudioEngine engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam, Reflections = false }, null);
        engine.RenderOffline(new float[4800 * 2]);
        Assert.False(engine.GetReflectionStats().Enabled);
        Assert.Empty(engine.GetReflectionSources());
    }
}

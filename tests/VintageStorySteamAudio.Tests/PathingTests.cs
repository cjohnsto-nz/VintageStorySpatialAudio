using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio.Tests;

/// <summary>Pathing (Phase 7): configuration and the engine's debugging views.</summary>
public sealed class PathingConfigTests
{
    [Fact]
    public void Pathing_is_on_by_default_and_its_settings_reach_the_engine_clamped()
    {
        var config = new SteamAudioConfig();
        Assert.True(config.Pathing);
        EngineOptions options = config.ToEngineOptions();
        Assert.True(options.Pathing);
        Assert.Equal(0, options.PathingSettings.RangeBlocks);  // the engine's default

        config = new SteamAudioConfig { Pathing = false, PathingRangeBlocks = 1000, PathingProbeSpacing = float.NaN, PathingSources = -3 };
        options = config.ToEngineOptions();
        Assert.False(options.Pathing);
        Assert.Equal(256, options.PathingSettings.RangeBlocks);
        Assert.Equal(0f, options.PathingSettings.ProbeSpacing);
        Assert.Equal(0, options.PathingSettings.Sources);
    }
}

/// <summary>Pathing through the real engine.</summary>
[Collection(NativeEngineGroup.Name)]
public sealed class PathingEngineTests
{
    [Fact]
    public void A_room_with_a_doorway_is_baked_and_a_blocked_sound_finds_its_way_out()
    {
        NativeTestEnvironment.RequireNatives();
        MaterialTable table = MaterialTable.Build(WorldSceneTests.ShippedConfig());
        using AudioEngine engine = AudioEngine.Create(new EngineOptions
        {
            RayTracer = RayTracer.Steam,
            Reflections = false,
            PathingSettings = new PathingSettings { RangeBlocks = 32, HeightBlocks = 16 },
        }, null);
        engine.SetSceneMaterials(table.Materials);

        // A stone room (interior 4..12 on x and z, floor at 2) with a doorway in its south wall,
        // and a floor outside it.
        var snapshot = new ChunkSnapshot();
        ushort stone = table.IdOf("stone");
        for (int z = 0; z < 32; z++)
        {
            for (int x = 0; x < 32; x++)
            {
                snapshot.Materials[(1 * 32 + z) * 32 + x] = stone;  // the ground
            }
        }

        for (int y = 2; y <= 6; y++)
        {
            for (int z = 3; z <= 12; z++)
            {
                for (int x = 3; x <= 12; x++)
                {
                    bool shell = x is 3 or 12 || z is 3 or 12 || y == 6;
                    bool doorway = z == 12 && x is 7 or 8 && y is 2 or 3;
                    if (shell && !doorway)
                    {
                        snapshot.Materials[(y * 32 + z) * 32 + x] = stone;
                    }
                }
            }
        }

        engine.SetSceneChunk(snapshot);
        Assert.True(engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));
        // Outside, south of the doorway, looking north at it; the sound inside, off to the west.
        engine.SetListener(8, 3.6f, 16, 0, 0, -1, 0, 1, 0);
        using AudioAsset tone = engine.CreatePcmAsset(NativeEngineTests.Sine(440, 48000, 48000, 0.3), 1, 48000, "tone");
        using Voice voice = engine.CreateVoice(tone, AudioBus.Sound, 1, 1, looping: true, new VoicePlacement(SpatialMode.World, 5, 3, 6));
        voice.Start();
        engine.RenderOffline(new float[48000 * 2]);

        PathingStats stats = engine.GetPathingStats();
        Assert.True(stats.Enabled);
        Assert.True(stats.Bakes >= 1);
        Assert.True(stats.Probes > 10);
        Assert.True(stats.Ticks > 3);
        Assert.Equal(1, stats.Wanted);
        Assert.Equal(1, stats.Found);
        Assert.NotEmpty(engine.GetPathSegments());
    }

    [Fact]
    public void Switched_off_there_is_no_pathing()
    {
        NativeTestEnvironment.RequireNatives();
        using AudioEngine engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam, Pathing = false }, null);
        engine.RenderOffline(new float[4800 * 2]);
        Assert.False(engine.GetPathingStats().Enabled);
        Assert.Empty(engine.GetPathSegments());
    }
}

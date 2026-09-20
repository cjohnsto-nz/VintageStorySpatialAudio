using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Tests;

/// <summary>The config file is rewritten with every default, so defaults that change need migrating.</summary>
public sealed class ConfigMigrationTests
{
    [Fact]
    public void A_file_from_before_the_reflection_gain_default_changed_gets_the_new_default()
    {
        var config = new SteamAudioConfig { ReflectionGain = 1f };  // ConfigVersion 0: the old default, as stored
        Assert.True(config.Migrate());
        Assert.Equal(0.1f, config.ReflectionGain);
        Assert.Equal(SteamAudioConfig.CurrentConfigVersion, config.ConfigVersion);
    }

    [Fact]
    public void A_gain_the_player_chose_is_kept()
    {
        var chosen = new SteamAudioConfig { ReflectionGain = 0.5f };
        Assert.True(chosen.Migrate());  // the version is stamped
        Assert.Equal(0.5f, chosen.ReflectionGain);

        var current = new SteamAudioConfig { ConfigVersion = SteamAudioConfig.CurrentConfigVersion, ReflectionGain = 1f };
        Assert.False(current.Migrate());
        Assert.Equal(1f, current.ReflectionGain);
    }

    [Fact]
    public void A_fresh_config_is_already_current()
    {
        var config = new SteamAudioConfig();
        config.Migrate();
        Assert.Equal(0.1f, config.ReflectionGain);
        Assert.Equal(SteamAudioConfig.CurrentConfigVersion, config.ConfigVersion);
    }

    private static EngineDefaults Defaults() => new(
        BlockFrames: 256, MaxVoices: 4096, MaxRealVoices: 256, MaxBinauralVoices: 64,
        OcclusionSamples: 16, OcclusionRateHz: 30,
        ReflectionSources: 10, ReflectionRays: 4096, ReflectionBounces: 16, ReflectionDurationSeconds: 1f,
        ReflectionOrder: 2, ReflectionRateHz: 10, ReflectionThreads: 3, ReflectionTransitionSeconds: 0.1f,
        PathingRangeBlocks: 64, PathingHeightBlocks: 64, PathingProbeSpacing: 2.5f, PathingVisibilitySamples: 1,
        PathingRateHz: 10, PathingSources: 16, PathingMaxProbes: 1200);

    [Fact]
    public void Every_setting_is_written_out_in_full_rather_than_left_at_zero()
    {
        var config = new SteamAudioConfig();
        Assert.Equal(0, config.ReflectionRays);  // as constructed: "the default", whatever it is
        Assert.True(config.Populate(Defaults()));

        // Medium's numbers, and the engine's where the preset has none.
        Assert.Equal(4096, config.ReflectionRays);
        Assert.Equal(10, config.ReflectionSources);
        Assert.Equal(3, config.ReflectionThreads);          // the machine's, from the engine
        Assert.Equal(64, config.PathingRangeBlocks);
        Assert.Equal(1200, config.PathingMaxProbes);
        Assert.Equal(2.5f, config.PathingProbeSpacing);
        Assert.Equal(16, config.OcclusionSamples);
        Assert.Equal(256, config.BlockFrames);
        Assert.Equal(ReflectionQuality.Medium, config.ReflectionQualityApplied);

        // Settled: a second pass changes nothing, so the file is not rewritten every launch.
        Assert.False(config.Populate(Defaults()));
    }

    [Fact]
    public void A_value_the_player_set_is_kept()
    {
        var config = new SteamAudioConfig { ReflectionRays = 1024, PathingMaxProbes = 400 };
        config.Populate(Defaults());
        Assert.Equal(1024, config.ReflectionRays);
        Assert.Equal(400, config.PathingMaxProbes);
        Assert.Equal(10, config.ReflectionSources);  // the ones they did not set are filled in
    }

    [Fact]
    public void Changing_the_quality_works_the_reflection_numbers_out_again()
    {
        var config = new SteamAudioConfig();
        config.Populate(Defaults());
        Assert.Equal(4096, config.ReflectionRays);

        // The player edits the preset in the file: the numbers it is responsible for follow,
        // and are not left pinned to the old preset's.
        config.ReflectionQuality = ReflectionQuality.Ultra;
        Assert.True(config.Populate(Defaults()));
        Assert.Equal(16384, config.ReflectionRays);
        Assert.Equal(32, config.ReflectionSources);
        Assert.Equal(ReflectionQuality.Ultra, config.ReflectionQualityApplied);
    }

    [Fact]
    public void Reset_restores_the_defaults_including_ones_that_changed_since()
    {
        var config = new SteamAudioConfig
        {
            ReflectionRays = 512, ReflectionGain = 2f, PathingRangeBlocks = 256, TakeOverGameAudio = false,
        };
        config.Populate(Defaults());
        config.ResetToDefaults(Defaults());
        Assert.Equal(4096, config.ReflectionRays);
        Assert.Equal(0.1f, config.ReflectionGain);
        Assert.Equal(64, config.PathingRangeBlocks);
        Assert.True(config.TakeOverGameAudio);
        Assert.Equal(SteamAudioConfig.CurrentConfigVersion, config.ConfigVersion);
    }

    [Fact]
    public void Populated_values_reach_the_engine_unchanged()
    {
        var config = new SteamAudioConfig();
        config.Populate(Defaults());
        EngineOptions options = config.ToEngineOptions();
        Assert.Equal(4096, options.ReflectionQuality.Rays);
        Assert.Equal(64, options.PathingSettings.RangeBlocks);
        Assert.Equal(1200, options.PathingSettings.MaxProbes);
    }
}

using VintageStorySteamAudio.Config;

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
}

using VintageStorySteamAudio.Diagnostics;

namespace VintageStorySteamAudio.Tests;

public sealed class TestPlaybackTests
{
    [Theory]
    [InlineData("effect/woodswitch", "game:sounds/effect/woodswitch.ogg")]
    [InlineData("sounds/effect/woodswitch", "game:sounds/effect/woodswitch.ogg")]
    [InlineData("game:sounds/music/menu.ogg", "game:sounds/music/menu.ogg")]
    [InlineData("mymod:beep.wav", "mymod:sounds/beep.wav")]
    public void Sound_names_map_to_asset_locations(string name, string expected) =>
        Assert.Equal(expected, TestPlayback.ToSoundLocation(name).ToString());
}

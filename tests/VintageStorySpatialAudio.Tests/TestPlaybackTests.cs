using VintageStorySpatialAudio.Diagnostics;

namespace VintageStorySpatialAudio.Tests;

public sealed class TestPlaybackTests
{
    [Theory]
    [InlineData("effect/woodswitch", "game:sounds/effect/woodswitch.ogg")]
    [InlineData("sounds/effect/woodswitch", "game:sounds/effect/woodswitch.ogg")]
    [InlineData("game:sounds/music/menu.ogg", "game:sounds/music/menu.ogg")]
    [InlineData("mymod:beep.wav", "mymod:sounds/beep.wav")]
    public void Sound_names_map_to_asset_locations(string name, string expected) =>
        Assert.Equal(expected, TestPlayback.ToSoundLocation(name).ToString());

    [Fact]
    public void Playat_says_whether_the_game_will_start_the_sound_at_that_distance()
    {
        // Under the takeover the range check is widened by SoundRangeMultiplier, so a range-110
        // wolf howl is started out to 660 m rather than 110.
        string near = VintageStorySpatialAudio.Diagnostics.WorldSoundTest.Describe(48.25, 110f, 6f);
        Assert.Contains("48.3 m from the listener", near, StringComparison.Ordinal);
        Assert.Contains("started out to 660.0 m", near, StringComparison.Ordinal);
        Assert.Contains("SoundRangeMultiplier 6", near, StringComparison.Ordinal);
        Assert.Contains("Started.", near, StringComparison.Ordinal);

        Assert.Contains("Too far", VintageStorySpatialAudio.Diagnostics.WorldSoundTest.Describe(660.1, 110f, 6f), StringComparison.Ordinal);
        Assert.Contains("Started.", VintageStorySpatialAudio.Diagnostics.WorldSoundTest.Describe(660.0, 110f, 6f), StringComparison.Ordinal);

        // Without the takeover the game's own range stands, and is named as such.
        string vanilla = VintageStorySpatialAudio.Diagnostics.WorldSoundTest.Describe(40.0, 32f, 1f);
        Assert.Contains("the game's own range", vanilla, StringComparison.Ordinal);
        Assert.Contains("Too far", vanilla, StringComparison.Ordinal);
    }
}

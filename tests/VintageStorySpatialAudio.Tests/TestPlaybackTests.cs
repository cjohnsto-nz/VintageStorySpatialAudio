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
    public void Playat_reports_what_the_game_did_not_what_the_distance_suggests()
    {
        const int Started = 1200;  // PlaySoundAt returns the length in ms, or 0 for nothing
        const int Nothing = 0;

        // Under the takeover the range check is widened, so a range-110 howl reaches 660 m.
        string near = WorldSoundTest.Describe(48.25, 110f, 6f, Started);
        Assert.Contains("48.3 m from the listener", near, StringComparison.Ordinal);
        Assert.Contains("started out to 660.0 m", near, StringComparison.Ordinal);
        Assert.Contains("SoundRangeMultiplier 6", near, StringComparison.Ordinal);
        Assert.Contains("The game started it.", near, StringComparison.Ordinal);

        Assert.Contains("past the range check", WorldSoundTest.Describe(660.1, 110f, 6f, Nothing), StringComparison.Ordinal);

        // In range but the game played nothing: the case a distance calculation cannot see, and
        // the one that made this command claim success while nothing sounded.
        string silent = WorldSoundTest.Describe(10.0, 110f, 6f, Nothing);
        Assert.Contains("though it is in range", silent, StringComparison.Ordinal);
        Assert.Contains("Audio File not found", silent, StringComparison.Ordinal);

        // Without the takeover the game's own range stands, and is named as such.
        Assert.Contains("the game's own range", WorldSoundTest.Describe(40.0, 32f, 1f, Nothing), StringComparison.Ordinal);
    }

    [Fact]
    public void A_sound_goes_to_the_game_under_sounds_and_without_an_extension()
    {
        // PlaySoundAt resolves wildcards and appends .ogg itself, but never adds the sounds/
        // prefix; passing it without gave "Audio File not found" and a silent command.
        Assert.Equal("game:sounds/creature/wolf/howl1", WorldSoundTest.ToGameSoundLocation("creature/wolf/howl1").ToString());
        Assert.Equal("game:sounds/creature/wolf/howl*", WorldSoundTest.ToGameSoundLocation(" creature/wolf/howl* ").ToString());
        Assert.Equal("game:sounds/creature/wolf/howl1", WorldSoundTest.ToGameSoundLocation("sounds/creature/wolf/howl1").ToString());
        Assert.Equal("mymod:sounds/thing", WorldSoundTest.ToGameSoundLocation("mymod:thing").ToString());
    }
}

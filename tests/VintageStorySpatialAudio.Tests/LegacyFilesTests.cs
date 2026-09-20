using VintageStorySpatialAudio.Config;

namespace VintageStorySpatialAudio.Tests;

public sealed class LegacyFilesTests : IDisposable
{
    private readonly string directory = Path.Combine(Path.GetTempPath(), "spatialaudio-legacy-" + Guid.NewGuid().ToString("N"));

    public LegacyFilesTests() => Directory.CreateDirectory(directory);

    public void Dispose() => Directory.Delete(directory, recursive: true);

    [Fact]
    public void Settings_written_under_the_old_name_are_carried_over_once_and_never_over_newer_ones()
    {
        File.WriteAllText(Path.Combine(directory, "vssteamaudio.json"), "{ \"ReflectionGain\": 0.25 }");
        File.WriteAllText(Path.Combine(directory, "vssteamaudio-materials.json"), "{ }");

        Assert.Equal(["spatialaudio.json", "spatialaudio-materials.json"], LegacyFiles.CarryOver(directory));
        Assert.Equal("{ \"ReflectionGain\": 0.25 }", File.ReadAllText(Path.Combine(directory, "spatialaudio.json")));
        Assert.True(File.Exists(Path.Combine(directory, "vssteamaudio.json")));  // left where it was

        // The new file is the player's from now on: the old one never replaces it.
        File.WriteAllText(Path.Combine(directory, "spatialaudio.json"), "{ \"ReflectionGain\": 0.5 }");
        Assert.Empty(LegacyFiles.CarryOver(directory));
        Assert.Equal("{ \"ReflectionGain\": 0.5 }", File.ReadAllText(Path.Combine(directory, "spatialaudio.json")));
    }

    [Fact]
    public void Nothing_to_carry_over_is_not_an_error()
    {
        Assert.Empty(LegacyFiles.CarryOver(directory));
    }
}

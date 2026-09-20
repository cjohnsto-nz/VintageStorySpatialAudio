namespace VintageStorySpatialAudio.Config;

/// <summary>
/// The mod was called "Steam Audio" (mod id vssteamaudio) before its first release. Settings and
/// material overrides written under that name are carried over once, so nobody who ran a
/// development build starts again from the defaults.
/// </summary>
public static class LegacyFiles
{
    private static readonly (string Old, string New)[] Renamed =
    [
        ("vssteamaudio.json", SpatialAudioConfig.FileName),
        ("vssteamaudio-materials.json", "spatialaudio-materials.json"),
    ];

    /// <summary>
    /// Copies each old file to its new name where the new one does not exist yet. The old files
    /// are left where they are. Returns the new names it wrote.
    /// </summary>
    public static IReadOnlyList<string> CarryOver(string modConfigDirectory)
    {
        var carried = new List<string>();
        foreach ((string old, string current) in Renamed)
        {
            string from = Path.Combine(modConfigDirectory, old);
            string to = Path.Combine(modConfigDirectory, current);
            if (File.Exists(from) && !File.Exists(to))
            {
                File.Copy(from, to);
                carried.Add(current);
            }
        }

        return carried;
    }
}

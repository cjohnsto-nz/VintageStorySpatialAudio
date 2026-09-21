using System.Text.RegularExpressions;

namespace VintageStorySpatialAudio.Takeover;

/// <summary>
/// How much of a sound still reaches the listener directly whatever is in the way, by the sound's
/// asset path (ADR 0022). Patterns take <c>*</c>; the first that matches wins, so the file's order
/// is the priority. A sound that matches nothing has no floor and is occluded as the world says.
/// </summary>
/// <remarks>
/// For calls that carry further than a model of the ground in front of the listener can show. A
/// wolf heard over a ridge is heard by diffraction, which Steam Audio models only along baked
/// paths, and those reach no further than the pathing box; past it the call is judged fully
/// occluded by the near terrain and vanishes.
/// </remarks>
public sealed class OcclusionFloors
{
    /// <summary>Nothing matches: every sound is occluded as the world says.</summary>
    public static readonly OcclusionFloors None = new([]);

    private readonly (Regex Pattern, float Floor)[] rules;

    private OcclusionFloors((Regex Pattern, float Floor)[] rules) => this.rules = rules;

    /// <summary>Whether any sound could be given a floor.</summary>
    public bool Any => rules.Length > 0;

    /// <summary>
    /// Builds the table. Patterns are matched against the asset path without its <c>sounds/</c>
    /// prefix, domain or extension, as the game's own sound names are written
    /// (<c>creature/wolf/howl1</c>). Floors outside 0..1, and empty patterns, are dropped through
    /// <paramref name="warn"/>.
    /// </summary>
    public static OcclusionFloors Build(IReadOnlyDictionary<string, float>? config, Action<string>? warn = null)
    {
        if (config is null || config.Count == 0)
        {
            return None;
        }

        var rules = new List<(Regex, float)>(config.Count);
        foreach ((string pattern, float floor) in config)
        {
            if (string.IsNullOrWhiteSpace(pattern))
            {
                warn?.Invoke("an occlusion floor has no sound pattern; ignored");
                continue;
            }

            if (!float.IsFinite(floor) || floor < 0f || floor > 1f)
            {
                warn?.Invoke($"the occlusion floor for '{pattern}' is {floor}, which is not within 0..1; ignored");
                continue;
            }

            string expression = "^" + Regex.Escape(Normalise(pattern)).Replace("\\*", ".*", StringComparison.Ordinal) + "$";
            rules.Add((new Regex(expression, RegexOptions.CultureInvariant | RegexOptions.IgnoreCase), floor));
        }

        return rules.Count == 0 ? None : new OcclusionFloors([.. rules]);
    }

    /// <summary>The floor for this sound, or 0 for none.</summary>
    public float For(string? assetLocation)
    {
        if (rules.Length == 0 || string.IsNullOrEmpty(assetLocation))
        {
            return 0f;
        }

        string path = Normalise(assetLocation);
        foreach ((Regex pattern, float floor) in rules)
        {
            if (pattern.IsMatch(path))
            {
                return floor;
            }
        }

        return 0f;
    }

    /// <summary>
    /// A sound as its name is written in the game's own assets: no domain, no <c>sounds/</c>
    /// prefix, no extension. The engine sees full asset locations, the player writes short names.
    /// </summary>
    internal static string Normalise(string location)
    {
        string path = location.Trim();
        int colon = path.IndexOf(':', StringComparison.Ordinal);
        if (colon >= 0)
        {
            path = path[(colon + 1)..];
        }

        if (path.StartsWith("sounds/", StringComparison.OrdinalIgnoreCase))
        {
            path = path["sounds/".Length..];
        }

        if (path.EndsWith(".ogg", StringComparison.OrdinalIgnoreCase) || path.EndsWith(".wav", StringComparison.OrdinalIgnoreCase))
        {
            path = path[..^4];
        }

        return path;
    }
}

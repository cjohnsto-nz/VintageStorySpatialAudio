using System.Text.RegularExpressions;

namespace VintageStorySpatialAudio.Takeover;

/// <summary>
/// A high-pass corner for chosen sounds, by the sound's asset path. Patterns take <c>*</c>; the
/// first that matches wins, so the file's order is the priority. A sound that matches nothing is
/// played as recorded.
/// </summary>
/// <remarks>
/// For recordings with more weight than the thing recorded. Footsteps chiefly: a creature's step
/// recorded close up booms, and the engine plays what it is given -- the low end reverberates and
/// passes through walls better than the rest, so it is what is left of the step at any distance.
/// </remarks>
public sealed class HighPassFilters
{
    /// <summary>The engine's limits (vsa_voice_desc::high_pass_hz).</summary>
    public const float MinHz = 20f;

    /// <inheritdoc cref="MinHz"/>
    public const float MaxHz = 2000f;

    /// <summary>Nothing matches: every sound is played as recorded.</summary>
    public static readonly HighPassFilters None = new([]);

    private readonly (Regex Pattern, float Hz)[] rules;

    private HighPassFilters((Regex Pattern, float Hz)[] rules) => this.rules = rules;

    /// <summary>Whether any sound could be filtered.</summary>
    public bool Any => rules.Length > 0;

    /// <summary>
    /// Builds the table. Patterns are matched against the asset path without its <c>sounds/</c>
    /// prefix, domain or extension, as the game's own sound names are written
    /// (<c>creature/wolf/footsteps/dirt/footstep-wolf-dirt1</c>). A corner of 0 switches a rule
    /// off (and, being first, exempts what it matches from the rules below it). Corners outside
    /// 20..2000 Hz, and empty patterns, are dropped through <paramref name="warn"/>.
    /// </summary>
    public static HighPassFilters Build(IReadOnlyDictionary<string, float>? config, Action<string>? warn = null)
    {
        if (config is null || config.Count == 0)
        {
            return None;
        }

        var rules = new List<(Regex, float)>(config.Count);
        foreach ((string pattern, float hz) in config)
        {
            if (string.IsNullOrWhiteSpace(pattern))
            {
                warn?.Invoke("a high-pass filter has no sound pattern; ignored");
                continue;
            }

            if (hz != 0f && (!float.IsFinite(hz) || hz < MinHz || hz > MaxHz))
            {
                warn?.Invoke($"the high-pass corner for '{pattern}' is {hz} Hz, which is not 0 or within {MinHz}..{MaxHz}; ignored");
                continue;
            }

            string expression = "^" + Regex.Escape(OcclusionFloors.Normalise(pattern)).Replace("\\*", ".*", StringComparison.Ordinal) + "$";
            rules.Add((new Regex(expression, RegexOptions.CultureInvariant | RegexOptions.IgnoreCase), hz));
        }

        return rules.Count == 0 ? None : new HighPassFilters([.. rules]);
    }

    /// <summary>The corner for this sound in Hz, or 0 for none.</summary>
    public float For(string? assetLocation)
    {
        if (rules.Length == 0 || string.IsNullOrEmpty(assetLocation))
        {
            return 0f;
        }

        string path = OcclusionFloors.Normalise(assetLocation);
        foreach ((Regex pattern, float hz) in rules)
        {
            if (pattern.IsMatch(path))
            {
                return hz;
            }
        }

        return 0f;
    }
}

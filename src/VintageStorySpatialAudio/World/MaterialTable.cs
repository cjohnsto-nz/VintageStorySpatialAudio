using System.Globalization;
using System.Text.RegularExpressions;
using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.World;

/// <summary>The acousticmaterials.json model.</summary>
public sealed class AcousticMaterialConfig
{
    public Dictionary<string, MaterialSpec> Materials { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public Dictionary<string, string> ByBlockMaterial { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public List<CodeRule> ByCode { get; set; } = [];
}

public sealed class MaterialSpec
{
    public string Kind { get; set; } = "Solid";

    public float[]? Absorption { get; set; }

    public float Scattering { get; set; } = 0.05f;

    public float[]? Transmission { get; set; }

    public float[]? AttenuationDbPerMetre { get; set; }

    public string? Color { get; set; }
}

public sealed class CodeRule
{
    public string Code { get; set; } = string.Empty;

    public string Material { get; set; } = string.Empty;
}

/// <summary>
/// The material table the engine gets (id = index, 0 = air) and how blocks map onto it: block
/// code rules first, then the block's material, then "generic".
/// </summary>
public sealed class MaterialTable
{
    public const string AirName = "air";
    public const string FallbackName = "generic";

    private readonly Dictionary<string, ushort> ids = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, ushort> byBlockMaterial = new(StringComparer.OrdinalIgnoreCase);
    private readonly List<(Regex Pattern, bool AnyDomain, ushort Material)> rules = [];
    private readonly List<AcousticMaterialDesc> materials = [];
    private readonly List<int> colors = [];

    private MaterialTable()
    {
    }

    /// <summary>In id order; id 0 is air.</summary>
    public IReadOnlyList<AcousticMaterialDesc> Materials => materials;

    /// <summary>Debug colours per id (0xAARRGGBB).</summary>
    public IReadOnlyList<int> Colors => colors;

    public int Count => materials.Count;

    /// <summary>Builds the table; problems (unknown names, bad values) go to <paramref name="warn"/> and fall back to defaults.</summary>
    public static MaterialTable Build(AcousticMaterialConfig config, Action<string>? warn = null)
    {
        ArgumentNullException.ThrowIfNull(config);
        var table = new MaterialTable();
        table.Add(AirName, new MaterialSpec { Kind = nameof(MaterialKind.Air), Color = "#000000" }, warn);
        foreach ((string name, MaterialSpec spec) in config.Materials ?? [])
        {
            if (!string.Equals(name, AirName, StringComparison.OrdinalIgnoreCase))
            {
                table.Add(name, spec ?? new MaterialSpec(), warn);
            }
        }

        if (!table.ids.ContainsKey(FallbackName))
        {
            table.Add(FallbackName, new MaterialSpec { Absorption = [0.1f, 0.2f, 0.3f], Transmission = [0.1f, 0.05f, 0.03f], AttenuationDbPerMetre = [20, 35, 50] }, warn);
        }

        foreach ((string blockMaterial, string name) in config.ByBlockMaterial ?? [])
        {
            table.byBlockMaterial[blockMaterial] = table.Lookup(name, $"byBlockMaterial.{blockMaterial}", warn);
        }

        foreach (CodeRule rule in config.ByCode ?? [])
        {
            if (string.IsNullOrWhiteSpace(rule.Code))
            {
                continue;
            }

            bool anyDomain = !rule.Code.Contains(':', StringComparison.Ordinal);
            string pattern = "^" + Regex.Escape(rule.Code.Trim().ToLowerInvariant()).Replace("\\*", ".*", StringComparison.Ordinal) + "$";
            table.rules.Add((new Regex(pattern, RegexOptions.CultureInvariant), anyDomain, table.Lookup(rule.Material, $"byCode {rule.Code}", warn)));
        }

        return table;
    }

    public ushort IdOf(string name) => ids.TryGetValue(name, out ushort id) ? id : ids[FallbackName];

    public bool TryGetId(string name, out ushort id) => ids.TryGetValue(name, out id);

    public string NameOf(ushort id) => id < materials.Count ? materials[id].Name : "?";

    public MaterialKind KindOf(ushort id) => id < materials.Count ? materials[id].Kind : MaterialKind.Air;

    /// <summary>The material for a block: its code ("domain:path") by rule, else its block material, else generic.</summary>
    public ushort Resolve(string code, string blockMaterial)
    {
        string lower = (code ?? string.Empty).ToLowerInvariant();
        int colon = lower.IndexOf(':', StringComparison.Ordinal);
        string path = colon >= 0 ? lower[(colon + 1)..] : lower;
        foreach ((Regex pattern, bool anyDomain, ushort material) in rules)
        {
            if (pattern.IsMatch(anyDomain ? path : lower))
            {
                return material;
            }
        }

        return byBlockMaterial.TryGetValue(blockMaterial ?? string.Empty, out ushort id) ? id : ids[FallbackName];
    }

    private ushort Lookup(string name, string where, Action<string>? warn)
    {
        if (ids.TryGetValue(name ?? string.Empty, out ushort id))
        {
            return id;
        }

        warn?.Invoke($"{where}: unknown material '{name}'; using {FallbackName}");
        return ids.TryGetValue(FallbackName, out ushort fallback) ? fallback : (ushort)0;
    }

    private void Add(string name, MaterialSpec spec, Action<string>? warn)
    {
        if (!Enum.TryParse(spec.Kind, ignoreCase: true, out MaterialKind kind) || !Enum.IsDefined(kind))
        {
            warn?.Invoke($"material {name}: unknown kind '{spec.Kind}'; using Solid");
            kind = MaterialKind.Solid;
        }

        ids[name] = (ushort)materials.Count;
        materials.Add(new AcousticMaterialDesc(
            name,
            kind,
            Bands(spec.Absorption, 0.1f, 0f, 1f, $"{name}.absorption", warn),
            Math.Clamp(spec.Scattering, 0f, 1f),
            Bands(spec.Transmission, 0f, 0f, 1f, $"{name}.transmission", warn),
            Bands(spec.AttenuationDbPerMetre, 0f, 0f, 1000f, $"{name}.attenuationDbPerMetre", warn)));
        colors.Add(ParseColor(spec.Color));
    }

    private static (float, float, float) Bands(float[]? values, float fallback, float min, float max, string where, Action<string>? warn)
    {
        if (values is null)
        {
            return (fallback, fallback, fallback);
        }

        if (values.Length != 3 || values.Any(v => !float.IsFinite(v)))
        {
            warn?.Invoke($"{where}: needs three finite values");
            return (fallback, fallback, fallback);
        }

        return (Math.Clamp(values[0], min, max), Math.Clamp(values[1], min, max), Math.Clamp(values[2], min, max));
    }

    private static int ParseColor(string? hex)
    {
        if (hex is { Length: 7 } && hex[0] == '#'
            && int.TryParse(hex.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int rgb))
        {
            return unchecked((int)0xFF000000) | rgb;
        }

        return unchecked((int)0xFFFF00FF);
    }
}

using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Config;

/// <summary>User configuration, stored as ModConfig/vssteamaudio.json.</summary>
/// <remarks>Phase 0 carries only engine bring-up options; quality presets arrive with the features they control.</remarks>
public sealed class SteamAudioConfig
{
    public const string FileName = "vssteamaudio.json";

    /// <summary>Auto, Embree or Steam.</summary>
    [JsonConverter(typeof(StringEnumConverter))]
    public RayTracer RayTracer { get; set; } = RayTracer.Auto;

    /// <summary>Enables Steam Audio's API validation layer. Slow; development only.</summary>
    public bool SteamAudioValidation { get; set; }

    /// <summary>Runs the native self-test when a world loads and logs the result.</summary>
    public bool RunSelfTestOnStartup { get; set; } = true;

    public EngineOptions ToEngineOptions() => new()
    {
        RayTracer = RayTracer,
        SteamAudioValidation = SteamAudioValidation,
    };
}

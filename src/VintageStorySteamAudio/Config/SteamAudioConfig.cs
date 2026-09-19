using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Config;

/// <summary>User configuration, stored as ModConfig/vssteamaudio.json.</summary>
/// <remarks>Engine bring-up and render options; quality presets arrive with the features they control.</remarks>
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

    /// <summary>Low, Medium (Default) or High: interpolation quality for sample-rate and pitch changes.</summary>
    [JsonConverter(typeof(StringEnumConverter))]
    public ResamplerQuality ResamplerQuality { get; set; } = ResamplerQuality.Default;

    /// <summary>Engine block size in frames (0 = 256). Smaller lowers latency and costs more CPU.</summary>
    public int BlockFrames { get; set; }

    /// <summary>Voice slot capacity (0 = 4096). A storage bound, not a cap on audible sounds.</summary>
    public int MaxVoices { get; set; }

    /// <summary>Part of a device name for the Phase 1 test commands; empty = system default.</summary>
    public string? TestOutputDevice { get; set; }

    public EngineOptions ToEngineOptions() => new()
    {
        RayTracer = RayTracer,
        SteamAudioValidation = SteamAudioValidation,
        ResamplerQuality = ResamplerQuality,
        BlockFrames = BlockFrames,
        MaxVoices = MaxVoices,
    };
}

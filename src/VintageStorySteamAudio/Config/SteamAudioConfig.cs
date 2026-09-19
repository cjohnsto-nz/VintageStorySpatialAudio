using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Config;

/// <summary>User configuration, stored as ModConfig/vssteamaudio.json.</summary>
/// <remarks>Engine bring-up and render options; quality presets arrive with the features they control.</remarks>
public sealed class SteamAudioConfig
{
    public const string FileName = "vssteamaudio.json";

    /// <summary>Play the game's sounds through Steam Audio in-world. False keeps vanilla OpenAL (the engine still starts, for diagnostics).</summary>
    public bool TakeOverGameAudio { get; set; } = true;

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

    /// <summary>Positional sounds rendered at once with their own Steam Audio effects (0 = 256); quieter ones go virtual.</summary>
    public int MaxRealVoices { get; set; }

    /// <summary>Of those, how many get their own HRTF with headphones (0 = 64); the rest share one binaural Ambisonic mix.</summary>
    public int MaxBinauralVoices { get; set; }

    /// <summary>
    /// Level trim per sound category in dB (Sound, Entity, Ambient, Weather, Music), applied on top
    /// of the game's sliders. Physical distance fall-off changes the balance vanilla was mixed for.
    /// </summary>
    public Dictionary<string, float> CategoryTrimDb { get; set; } = new()
    {
        ["Sound"] = 0f,
        ["Entity"] = 0f,
        ["Ambient"] = 0f,
        ["Weather"] = 0f,
        ["Music"] = 0f,
    };

    /// <summary>
    /// Metres the listener sits behind the player's eyes (horizontally), so the player's own sounds
    /// come from the front rather than flipping to the rear speakers. 0 disables it.
    /// </summary>
    public float ListenerBackwardOffset { get; set; } = 0.5f;

    /// <summary>Part of a device name for the .steamaudio play test command; empty = the device in use or the system default.</summary>
    public string? TestOutputDevice { get; set; }

    public EngineOptions ToEngineOptions() => new()
    {
        RayTracer = RayTracer,
        SteamAudioValidation = SteamAudioValidation,
        ResamplerQuality = ResamplerQuality,
        BlockFrames = BlockFrames,
        MaxVoices = MaxVoices,
        MaxRealVoices = MaxRealVoices,
        MaxBinauralVoices = MaxBinauralVoices,
    };

    /// <summary>The trims by bus; unknown names are ignored.</summary>
    public IReadOnlyDictionary<AudioBus, float> CategoryTrimsDb()
    {
        var trims = new Dictionary<AudioBus, float>();
        foreach ((string name, float db) in CategoryTrimDb ?? [])
        {
            if (Enum.TryParse(name, ignoreCase: true, out AudioBus bus) && float.IsFinite(db))
            {
                trims[bus] = Math.Clamp(db, -40f, 20f);
            }
        }

        return trims;
    }
}

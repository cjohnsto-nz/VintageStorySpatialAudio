using System.Globalization;
using Newtonsoft.Json;
using Newtonsoft.Json.Converters;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Config;

/// <summary>User configuration, stored as ModConfig/vssteamaudio.json.</summary>
/// <remarks>Engine bring-up and render options; quality presets arrive with the features they control.</remarks>
public sealed class SteamAudioConfig
{
    public const string FileName = "vssteamaudio.json";

    /// <summary>The defaults this file was written with; see <see cref="Migrate"/>.</summary>
    public const int CurrentConfigVersion = 1;

    /// <summary>Which version of the defaults this file was written with. Left alone.</summary>
    public int ConfigVersion { get; set; }

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

    /// <summary>
    /// With speakers (the game's HRTF option off), play through Windows Spatial Audio when the
    /// output device has a spatial sound format enabled (Dolby Atmos for home theater, DTS:X):
    /// a 7.1.4 mix, so sounds above reach the height speakers. Otherwise the device is used directly.
    /// </summary>
    public bool SpatialAudio { get; set; } = true;

    /// <summary>
    /// A SOFA file with a personal HRTF for headphones (absolute, or relative to the ModConfig
    /// folder); empty = Steam Audio's default. Falls back to the default if it cannot be loaded.
    /// </summary>
    public string? HrtfSofaFile { get; set; }

    /// <summary>Builds the acoustic scene from the world around the listener (Phase 4).</summary>
    public bool BuildWorldScene { get; set; } = true;

    /// <summary>Chunks (horizontally) around the listener's chunk at full detail.</summary>
    public int SceneFullRadiusChunks { get; set; } = 2;

    /// <summary>Chunks around the listener's chunk in the scene at all (beyond the full-detail radius: 2x2x2-block detail).</summary>
    public int SceneLodRadiusChunks { get; set; } = 4;

    /// <summary>Chunks above and below the listener's chunk in the scene.</summary>
    public int SceneVerticalRadiusChunks { get; set; } = 2;

    /// <summary>Main-thread milliseconds per game tick (every 50 ms) spent reading chunks for the scene.</summary>
    public double SceneBudgetMs { get; set; } = 2.0;

    /// <summary>
    /// Vanilla never plays a sound beyond its range (an anvil stops dead at 16 m); with physical
    /// fall-off sounds should fade instead. Ranges are multiplied by this (1 = vanilla, up to 16).
    /// </summary>
    public float SoundRangeMultiplier { get; set; } = 3f;

    /// <summary>
    /// How far a sound carries. Every sound's reference distance -- the distance within which it
    /// plays at full volume -- is multiplied by this (0.25 to 8). The fall-off stays physical
    /// beyond it (inverse distance, about 6 dB quieter each time the distance doubles), so this
    /// moves the whole curve outwards rather than bending it: 2 is roughly 6 dB more at every
    /// distance past the reference, and twice the distance for a given loudness.
    ///
    /// Raise <see cref="SoundRangeMultiplier"/> alongside it, or the game will still not start
    /// sounds that are far away. Louder distant sounds also stay simulated for longer, so more
    /// of them hold Steam Audio effects at once.
    /// </summary>
    public float ReferenceDistanceMultiplier { get; set; } = 1f;

    /// <summary>
    /// Sounds a creature or player makes follow them while they play. Vanilla leaves each sound where
    /// it started, so a running wolf's growl stays behind it.
    /// </summary>
    public bool TrackEntitySounds { get; set; } = true;

    /// <summary>
    /// Also for creature sounds the server sends as plain coordinates (most calls, even in single
    /// player): each is matched to the creature it came from, when only one is close enough.
    /// </summary>
    public bool InferEntitySounds { get; set; } = true;

    /// <summary>How far (blocks, 0..4) such a sound may be from a creature's body to be matched to it.</summary>
    public float EntitySoundMatchDistance { get; set; } = 1f;

    /// <summary>Walls muffle what is behind them (occlusion and transmission by the world scene, Phase 5).</summary>
    public bool Occlusion { get; set; } = true;

    /// <summary>Rays per sound for occlusion (more: smoother edges, more CPU). 0 = 16.</summary>
    public int OcclusionSamples { get; set; }

    /// <summary>Occlusion updates per second. 0 = 30.</summary>
    public int OcclusionRateHz { get; set; }

    /// <summary>
    /// Reverb from the world itself (Phase 6): rooms, caves and halls ring as their size and
    /// materials make them. Replaces vanilla's reverb presets.
    /// </summary>
    public bool Reflections { get; set; } = true;

    /// <summary>
    /// The preset the reflection numbers below were worked out from. The mod sets it; change
    /// <see cref="ReflectionQuality"/> and they are worked out again from the new preset.
    /// </summary>
    public ReflectionQuality? ReflectionQualityApplied { get; set; }

    /// <summary>Low, Medium, High or Ultra (see <see cref="ReflectionPresets"/>); the settings below override single values.</summary>
    [JsonConverter(typeof(StringEnumConverter))]
    public ReflectionQuality ReflectionQuality { get; set; } = ReflectionQuality.Medium;

    /// <summary>Places simulated at once (every sound is simulated from where it is; sounds within 3 m share a place); 0 = the preset's.</summary>
    public int ReflectionSources { get; set; }

    /// <summary>Rays per simulation; 0 = the preset's.</summary>
    public int ReflectionRays { get; set; }

    /// <summary>Bounces per ray; 0 = the preset's.</summary>
    public int ReflectionBounces { get; set; }

    /// <summary>Longest reverb simulated, seconds; 0 = the preset's.</summary>
    public float ReflectionDurationSeconds { get; set; }

    /// <summary>Ambisonic order of the reflections (1..3, direction detail); 0 = the preset's.</summary>
    public int ReflectionOrder { get; set; }

    /// <summary>Simulations per second at most; 0 = the preset's.</summary>
    public int ReflectionRateHz { get; set; }

    /// <summary>Threads per simulation; 0 = the preset's (a quarter of the cores, up to 4).</summary>
    public int ReflectionThreads { get; set; }

    /// <summary>Seconds of early reflections rendered exactly (directional); 0 = the preset's.</summary>
    public float ReflectionTransitionSeconds { get; set; }

    /// <summary>
    /// Sound round corners and through doorways (Phase 7): Steam Audio's baked pathing over a box
    /// round you, baked in the background (about half a second per 64³ of world).
    /// </summary>
    public bool Pathing { get; set; } = true;

    /// <summary>The box baked round you, blocks across; 0 = 96 (32..256). Bigger takes longer to bake.</summary>
    public int PathingRangeBlocks { get; set; }

    /// <summary>The box's height, blocks; 0 = 64 (16..128).</summary>
    public int PathingHeightBlocks { get; set; }

    /// <summary>Metres between probes; 0 = 2.5 (1..8). Closer follows narrow passages better and bakes slower.</summary>
    public float PathingProbeSpacing { get; set; }

    /// <summary>Rays per probe when testing whether two probes see each other; 0 = 1 (1..8).</summary>
    public int PathingVisibilitySamples { get; set; }

    /// <summary>Path simulations per second; 0 = 10 (1..60).</summary>
    public int PathingRateHz { get; set; }

    /// <summary>Blocked sounds given paths per simulation at most; 0 = 16 (1..256).</summary>
    public int PathingSources { get; set; }

    /// <summary>
    /// Probes baked at most (0 = 1200). A bake costs about probes^2.2 and how many a box holds
    /// is up to the terrain, so the spacing widens to stay within this. Raising it buys finer
    /// paths in complex ground at a bake cost that climbs steeply.
    /// </summary>
    public int PathingMaxProbes { get; set; }

    /// <summary>
    /// Scales all reverb (0..4; also .steamaudio reverb gain). 1 is the level Steam Audio
    /// simulates; the default is tuned by ear on a 7.1.4 system, where the simulated level is
    /// far too much on top of the game's own sound mix.
    /// </summary>
    public float ReflectionGain { get; set; } = 0.1f;

    /// <summary>Scales the early reflections, the first 0.1 s or so (0 = off, 0..4; also .steamaudio reverb early).</summary>
    public float ReflectionEarlyGain { get; set; } = 1f;

    /// <summary>Scales the reverb tail after them (0 = off, 0..4; also .steamaudio reverb tail).</summary>
    public float ReflectionTailGain { get; set; } = 1f;

    /// <summary>Part of a device name for the .steamaudio play test command; empty = the device in use or the system default.</summary>
    public string? TestOutputDevice { get; set; }

    /// <param name="modConfigDirectory">Where relative paths (HrtfSofaFile) are resolved; null leaves them as they are.</param>
    public EngineOptions ToEngineOptions(string? modConfigDirectory = null) => new()
    {
        RayTracer = RayTracer,
        SteamAudioValidation = SteamAudioValidation,
        ResamplerQuality = ResamplerQuality,
        BlockFrames = BlockFrames,
        MaxVoices = MaxVoices,
        MaxRealVoices = MaxRealVoices,
        MaxBinauralVoices = MaxBinauralVoices,
        DirectSimulation = Occlusion,
        OcclusionSamples = Math.Clamp(OcclusionSamples, 0, 256),
        DirectRateHz = Math.Clamp(OcclusionRateHz, 0, 120),
        Reflections = Reflections,
        ReflectionQuality = ReflectionPresets.Resolve(ReflectionQuality, new ReflectionQualitySettings
        {
            Sources = ReflectionSources,
            Rays = ReflectionRays,
            Bounces = ReflectionBounces,
            DurationSeconds = ReflectionDurationSeconds,
            Order = ReflectionOrder,
            RateHz = ReflectionRateHz,
            Threads = ReflectionThreads,
            TransitionSeconds = ReflectionTransitionSeconds,
        }),
        Pathing = Pathing,
        PathingSettings = new PathingSettings
        {
            RangeBlocks = Math.Clamp(PathingRangeBlocks, 0, 256),
            HeightBlocks = Math.Clamp(PathingHeightBlocks, 0, 128),
            ProbeSpacing = float.IsFinite(PathingProbeSpacing) ? Math.Clamp(PathingProbeSpacing, 0f, 8f) : 0f,
            VisibilitySamples = Math.Clamp(PathingVisibilitySamples, 0, 8),
            RateHz = Math.Clamp(PathingRateHz, 0, 60),
            Sources = Math.Clamp(PathingSources, 0, 256),
            MaxProbes = Math.Clamp(PathingMaxProbes, 0, 65536),
        },
        HrtfSofaPath = string.IsNullOrWhiteSpace(HrtfSofaFile) ? null
            : modConfigDirectory is null ? HrtfSofaFile.Trim()
            : Path.GetFullPath(HrtfSofaFile.Trim(), modConfigDirectory),
    };

    /// <summary>
    /// Brings a file written by an earlier version up to date. The file is rewritten with every
    /// default, so a default that changed since is replaced where the file still holds the old
    /// one; a value the player chose is kept. Returns true if anything changed.
    /// </summary>
    public bool Migrate()
    {
        bool changed = false;
        if (ConfigVersion < 1 && Math.Abs(ReflectionGain - 1f) < 1e-6f)
        {
            ReflectionGain = 0.1f;  // the default until version 1 was 1 (as simulated): far too loud
            changed = true;
        }

        if (ConfigVersion != CurrentConfigVersion)
        {
            ConfigVersion = CurrentConfigVersion;
            changed = true;
        }

        return changed;
    }

    /// <summary>
    /// Writes the effective value into every setting that is still unset, so the file says what
    /// the engine will actually do rather than leaving zeros to stand for "the default"
    /// (ADR 0018). Reflection values come from <see cref="ReflectionQuality"/>'s preset, the
    /// rest from the engine; a value already set is kept. Changing the quality works the
    /// reflection numbers out again. Returns true if anything changed.
    /// </summary>
    public bool Populate(EngineDefaults defaults)
    {
        ArgumentNullException.ThrowIfNull(defaults);
        string before = Fingerprint();
        if (ReflectionQualityApplied is not null && ReflectionQualityApplied != ReflectionQuality)
        {
            // A new preset replaces the numbers it is responsible for. Only once one has been
            // applied: on a file from before this, the values in it are the player's own.
            ReflectionSources = 0;
            ReflectionRays = 0;
            ReflectionBounces = 0;
            ReflectionDurationSeconds = 0f;
            ReflectionOrder = 0;
            ReflectionRateHz = 0;
            ReflectionThreads = 0;
            ReflectionTransitionSeconds = 0f;
        }

        ReflectionQualityApplied = ReflectionQuality;

        ReflectionQualitySettings reflections = ReflectionPresets.Resolve(ReflectionQuality, new ReflectionQualitySettings
        {
            Sources = ReflectionSources,
            Rays = ReflectionRays,
            Bounces = ReflectionBounces,
            DurationSeconds = ReflectionDurationSeconds,
            Order = ReflectionOrder,
            RateHz = ReflectionRateHz,
            Threads = ReflectionThreads,
            TransitionSeconds = ReflectionTransitionSeconds,
        });

        BlockFrames = Keep(BlockFrames, defaults.BlockFrames);
        MaxVoices = Keep(MaxVoices, defaults.MaxVoices);
        MaxRealVoices = Keep(MaxRealVoices, defaults.MaxRealVoices);
        MaxBinauralVoices = Keep(MaxBinauralVoices, defaults.MaxBinauralVoices);
        OcclusionSamples = Keep(OcclusionSamples, defaults.OcclusionSamples);
        OcclusionRateHz = Keep(OcclusionRateHz, defaults.OcclusionRateHz);

        ReflectionSources = Keep(reflections.Sources, defaults.ReflectionSources);
        ReflectionRays = Keep(reflections.Rays, defaults.ReflectionRays);
        ReflectionBounces = Keep(reflections.Bounces, defaults.ReflectionBounces);
        ReflectionDurationSeconds = KeepF(reflections.DurationSeconds, defaults.ReflectionDurationSeconds);
        ReflectionOrder = Keep(reflections.Order, defaults.ReflectionOrder);
        ReflectionRateHz = Keep(reflections.RateHz, defaults.ReflectionRateHz);
        ReflectionThreads = Keep(reflections.Threads, defaults.ReflectionThreads);
        ReflectionTransitionSeconds = KeepF(reflections.TransitionSeconds, defaults.ReflectionTransitionSeconds);

        PathingRangeBlocks = Keep(PathingRangeBlocks, defaults.PathingRangeBlocks);
        PathingHeightBlocks = Keep(PathingHeightBlocks, defaults.PathingHeightBlocks);
        PathingProbeSpacing = KeepF(PathingProbeSpacing, defaults.PathingProbeSpacing);
        PathingVisibilitySamples = Keep(PathingVisibilitySamples, defaults.PathingVisibilitySamples);
        PathingRateHz = Keep(PathingRateHz, defaults.PathingRateHz);
        PathingSources = Keep(PathingSources, defaults.PathingSources);
        PathingMaxProbes = Keep(PathingMaxProbes, defaults.PathingMaxProbes);
        return Fingerprint() != before;

        static int Keep(int value, int fallback) => value > 0 ? value : fallback;
        static float KeepF(float value, float fallback) => float.IsFinite(value) && value > 0f ? value : fallback;
    }

    /// <summary>Every setting back to its default, keeping only the quality preset chosen.</summary>
    public void ResetToDefaults(EngineDefaults defaults)
    {
        ArgumentNullException.ThrowIfNull(defaults);
        var fresh = new SteamAudioConfig { ReflectionQuality = ReflectionQuality };
        foreach (System.Reflection.PropertyInfo p in typeof(SteamAudioConfig).GetProperties())
        {
            if (p.CanRead && p.CanWrite)
            {
                p.SetValue(this, p.GetValue(fresh));
            }
        }

        ConfigVersion = CurrentConfigVersion;
        Populate(defaults);
    }

    /// <summary>Everything that decides what the engine does, for telling one config from another.</summary>
    private string Fingerprint() => string.Create(
        CultureInfo.InvariantCulture,
        $"{ConfigVersion} {BlockFrames} {MaxVoices} {MaxRealVoices} {MaxBinauralVoices} {OcclusionSamples} {OcclusionRateHz} " +
        $"{ReflectionQualityApplied} {ReflectionSources} {ReflectionRays} {ReflectionBounces} {ReflectionDurationSeconds} " +
        $"{ReflectionOrder} {ReflectionRateHz} {ReflectionThreads} {ReflectionTransitionSeconds} " +
        $"{PathingRangeBlocks} {PathingHeightBlocks} {PathingProbeSpacing} {PathingVisibilitySamples} {PathingRateHz} " +
        $"{PathingSources} {PathingMaxProbes}");

    /// <summary>The reflection gain, clamped to what the engine accepts.</summary>
    public float ReflectionGainClamped() => ClampGain(ReflectionGain);

    /// <summary>The early reflections' and the tail's gains, clamped to what the engine accepts.</summary>
    public (float Early, float Tail) ReflectionMixClamped() => (ClampGain(ReflectionEarlyGain), ClampGain(ReflectionTailGain));

    private static float ClampGain(float gain) => float.IsFinite(gain) ? Math.Clamp(gain, 0f, 4f) : 1f;

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

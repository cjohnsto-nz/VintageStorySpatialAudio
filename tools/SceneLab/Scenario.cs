using System.Text.Json;
using System.Text.Json.Serialization;
using VintageStorySteamAudio.Native;

namespace SceneLab;

/// <summary>A scene to render: assets, voices with timed actions, and optional pass/fail expectations.</summary>
public sealed record Scenario
{
    public string Name { get; init; } = "scenario";

    public int SampleRate { get; init; } = 48000;

    public int Channels { get; init; } = 2;

    public double DurationSeconds { get; init; } = 5;

    public EngineSettings Engine { get; init; } = new();

    public float MasterGain { get; init; } = 1f;

    /// <summary>Headphones (binaural for the loudest positional voices) or Speakers (panning).</summary>
    public RenderMode RenderMode { get; init; } = RenderMode.Headphones;

    /// <summary>Listener at the origin facing -z unless set: position, forward, up.</summary>
    public ListenerSpec? Listener { get; init; }

    /// <summary>Bus name (Sound, Entity, Ambient, Weather, Music) to gain.</summary>
    public IReadOnlyDictionary<string, float> Buses { get; init; } = new Dictionary<string, float>();

    public IReadOnlyDictionary<string, AssetSpec> Assets { get; init; } = new Dictionary<string, AssetSpec>();

    public IReadOnlyList<VoiceSpec> Voices { get; init; } = [];

    public Expectations? Expect { get; init; }

    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter() },
    };

    public static Scenario Load(string path) =>
        JsonSerializer.Deserialize<Scenario>(File.ReadAllText(path), JsonOptions)
        ?? throw new InvalidDataException($"{path} is empty");
}

public sealed record EngineSettings
{
    public int BlockFrames { get; init; }

    public int MaxVoices { get; init; }

    public ResamplerQuality ResamplerQuality { get; init; } = ResamplerQuality.Default;

    public int StreamThresholdMs { get; init; }

    public int MaxRealVoices { get; init; }

    public int MaxBinauralVoices { get; init; }
}

public sealed record ListenerSpec
{
    public IReadOnlyList<float> Position { get; init; } = [0, 0, 0];

    public IReadOnlyList<float> Forward { get; init; } = [0, 0, -1];

    public IReadOnlyList<float> Up { get; init; } = [0, 1, 0];
}

/// <summary>Circular motion in the horizontal plane around a centre, updated every block.</summary>
public sealed record OrbitSpec
{
    public IReadOnlyList<float> Centre { get; init; } = [0, 0, 0];

    public float Radius { get; init; } = 3f;

    /// <summary>Seconds per revolution; negative turns the other way.</summary>
    public float PeriodSeconds { get; init; } = 4f;
}

/// <summary>An audio file (relative to the scenario) or a generated signal.</summary>
public sealed record AssetSpec
{
    public string? File { get; init; }

    public AssetStorage Storage { get; init; } = AssetStorage.Auto;

    public ToneSpec? Tone { get; init; }

    public NoiseSpec? Noise { get; init; }
}

public enum ToneShape
{
    Sine,
    Square,
    Saw,
}

public sealed record ToneSpec
{
    public double Frequency { get; init; } = 440;

    public double Seconds { get; init; } = 1;

    public int SampleRate { get; init; } = 48000;

    public int Channels { get; init; } = 1;

    public double Amplitude { get; init; } = 0.5;

    public ToneShape Shape { get; init; } = ToneShape.Sine;
}

public sealed record NoiseSpec
{
    public double Seconds { get; init; } = 1;

    public int SampleRate { get; init; } = 48000;

    public int Channels { get; init; } = 1;

    public double Amplitude { get; init; } = 0.3;

    public int Seed { get; init; } = 1;
}

public sealed record VoiceSpec
{
    public required string Asset { get; init; }

    public AudioBus Bus { get; init; } = AudioBus.Sound;

    public float Gain { get; init; } = 1f;

    public float Pitch { get; init; } = 1f;

    public bool Loop { get; init; }

    /// <summary>When to start the voice, in seconds; null creates it stopped.</summary>
    public double? Start { get; init; } = 0;

    /// <summary>Create this many copies (load tests).</summary>
    public int Count { get; init; } = 1;

    /// <summary>Copies get pitches spread evenly over pitch +- pitchSpread.</summary>
    public float PitchSpread { get; init; }

    /// <summary>Copy i starts i * startStagger seconds later.</summary>
    public double StartStagger { get; init; }

    public IReadOnlyList<VoiceAction> Actions { get; init; } = [];

    /// <summary>None (default), World or Listener.</summary>
    public SpatialMode Spatial { get; init; } = SpatialMode.None;

    public IReadOnlyList<float> Position { get; init; } = [0, 0, 0];

    public float MinDistance { get; init; }

    /// <summary>Moves the voice around a circle (implies World positioning).</summary>
    public OrbitSpec? Orbit { get; init; }
}

/// <summary>Something that happens to a voice at time <see cref="At"/>. Set exactly one member.</summary>
public sealed record VoiceAction
{
    public double At { get; init; }

    public bool? Start { get; init; }

    public bool? Pause { get; init; }

    public bool? Stop { get; init; }

    public bool? Release { get; init; }

    public float? Gain { get; init; }

    public float? Pitch { get; init; }

    public bool? Loop { get; init; }

    public double? Seek { get; init; }

    public FadeSpec? Fade { get; init; }

    /// <summary>New position (keeps the voice's spatial mode).</summary>
    public IReadOnlyList<float>? Position { get; init; }

    /// <summary>High-frequency damping, 0..1 (the game uses 0.06 underwater).</summary>
    public float? Lowpass { get; init; }
}

public sealed record FadeSpec
{
    public float To { get; init; }

    public float Seconds { get; init; } = 1f;

    public bool Stop { get; init; }
}

/// <summary>Pass/fail limits; SceneLab exits non-zero when one is exceeded.</summary>
public sealed record Expectations
{
    public ulong? MaxOverloads { get; init; }

    public ulong? MaxUnderruns { get; init; }

    public double? MaxPeakDbfs { get; init; }

    public double? MinRmsDbfs { get; init; }

    /// <summary>Largest allowed render time relative to audio time (p99 block).</summary>
    public double? MaxP99Load { get; init; }
}

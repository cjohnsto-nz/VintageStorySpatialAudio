using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace VintageStorySteamAudio.Native;

// Managed mirror of native/include/vsaudio.h. Keep the two in lockstep:
//  - structs are blittable and field-for-field identical,
//  - enum-valued struct fields are uint (see the header's rules),
//  - AbiVersion must equal VSA_ABI_VERSION.
// Layout is asserted by NativeLayoutTests and at runtime by the native side (struct_size).

internal enum VsaResult
{
    Ok = 0,
    InvalidArgument = 1,
    AbiMismatch = 2,
    AlreadyExists = 3,
    SteamAudio = 4,
    OutOfMemory = 5,
    Unsupported = 6,
    Internal = 7,
    InvalidHandle = 8,
    Decode = 9,
    Device = 10,
    InvalidState = 11,
    Capacity = 12,
}

internal enum VsaLogLevel
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
}

/// <summary>Which ray tracer backs Steam Audio scenes.</summary>
public enum RayTracer : uint
{
    /// <summary>Embree when available on this machine, otherwise Steam Audio's built-in tracer.</summary>
    Auto = 0,
    /// <summary>Intel Embree; engine creation fails if it is unavailable.</summary>
    Embree = 1,
    /// <summary>Steam Audio's built-in ray tracer. Always available.</summary>
    Steam = 2,
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaVersionInfo
{
    public uint StructSize;
    public uint AbiVersion;
    public uint EngineMajor;
    public uint EngineMinor;
    public uint EnginePatch;
    public uint SteamAudioMajor;
    public uint SteamAudioMinor;
    public uint SteamAudioPatch;
    public nint BuildDescription;
}

/// <summary>Resampler quality: zero crossings per side of the interpolation kernel.</summary>
public enum ResamplerQuality : uint
{
    /// <summary>Medium.</summary>
    Default = 0,
    /// <summary>4 zero crossings.</summary>
    Low = 1,
    /// <summary>8 zero crossings.</summary>
    Medium = 2,
    /// <summary>16 zero crossings.</summary>
    High = 3,
}

/// <summary>Mix buses; they mirror the game's sound categories.</summary>
public enum AudioBus : uint
{
    Sound = 0,
    Entity = 1,
    Ambient = 2,
    Weather = 3,
    Music = 4,
}

public enum AssetFormat : uint
{
    /// <summary>Detect Ogg Vorbis or RIFF WAVE from the data.</summary>
    Auto = 0,
    OggVorbis = 1,
    Wav = 2,
    /// <summary>Raw interleaved signed 16-bit PCM.</summary>
    PcmS16 = 3,
}

public enum AssetStorage : uint
{
    /// <summary>Stream Ogg assets longer than the engine's threshold, decode everything else.</summary>
    Auto = 0,
    Decoded = 1,
    Streamed = 2,
}

public enum VoiceState : uint
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
}

public enum OutputKind : uint
{
    /// <summary>No device: audio only through <see cref="AudioEngine.RenderOffline"/>.</summary>
    None = 0,
    Device = 1,
}

public enum EngineEventType : uint
{
    FadeDone = 1,
    VoiceEnded = 2,
    StreamUnderrun = 3,
    DeviceRerouted = 4,
    DeviceLost = 5,
    DeviceRestored = 6,
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaEngineConfig
{
    public uint StructSize;
    public uint AbiVersion;
    public delegate* unmanaged[Cdecl]<nint, VsaLogLevel, byte*, void> Log;
    public nint LogUserData;
    public uint RayTracer;
    public uint Flags;
    public uint SampleRate;
    public uint BlockFrames;
    public uint MaxVoices;
    public uint ResamplerQuality;
    public uint StreamThresholdMs;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaAssetDesc
{
    public uint StructSize;
    public uint Format;
    public void* Data;
    public ulong Size;
    public byte* Name;
    public uint Storage;
    public uint PcmChannels;
    public uint PcmSampleRate;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaAssetInfo
{
    public uint StructSize;
    public uint Channels;
    public uint SampleRate;
    public uint Storage;
    public ulong Frames;
    public ulong MemoryBytes;
    public double DurationSeconds;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaVoiceDesc
{
    public uint StructSize;
    public uint Bus;
    public nint Asset;
    public float Gain;
    public float Pitch;
    public uint Looping;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaVoiceStatus
{
    public uint StructSize;
    public uint State;
    public double PositionSeconds;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaDeviceId
{
    public const int Length = 512;
    public fixed byte Bytes[Length];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaDeviceInfo
{
    public const int NameLength = 256;
    public uint StructSize;
    public uint IsDefault;
    public fixed byte Name[NameLength];
    public VsaDeviceId Id;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaOutputDesc
{
    public uint StructSize;
    public uint Kind;
    public VsaDeviceId* DeviceId;
    public uint Channels;
    public uint SampleRate;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaEngineStats
{
    public uint StructSize;
    public uint OutputKind;
    public uint SampleRate;
    public uint Channels;
    public uint BlockFrames;
    public uint DevicePeriodFrames;
    public uint ActiveVoices;
    public uint AllocatedVoices;
    public uint MaxVoices;
    public float LimiterPeakReductionDb;
    public ulong BlocksRendered;
    public ulong Overloads;
    public ulong StreamUnderruns;
    public ulong EventsDropped;
    public double RenderTimeAvgUs;
    public double RenderTimeMaxUs;
    public double BlockPeriodUs;
    public fixed byte DeviceName[256];
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaEvent
{
    public uint StructSize;
    public uint Type;
    public ulong Voice;
    public ulong Token;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaEngineInfo
{
    public uint StructSize;
    public uint ActiveRayTracer;
    public uint EmbreeAvailable;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaSelfTestReport
{
    public uint StructSize;
    public uint Passed;
    public float OcclusionThroughWall;
    public float OcclusionClearPath;
    public double ElapsedMs;
}

internal static unsafe partial class VsaNative
{
    public const string LibraryName = "vsaudio";

    /// <summary>Must equal VSA_ABI_VERSION in vsaudio.h.</summary>
    public const uint AbiVersion = 2;

    public const uint EngineFlagSteamAudioValidation = 1u << 0;
    public const uint FadeStopWhenDone = 1u << 0;
    public const uint EventFlagFadeCancelled = 1u << 0;
    public const int BusCount = 5;

    [LibraryImport(LibraryName, EntryPoint = "vsa_get_version")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult GetVersion(ref VsaVersionInfo info);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineCreate(in VsaEngineConfig config, out nint engine);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_destroy")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void EngineDestroy(nint engine);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_info")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetInfo(nint engine, ref VsaEngineInfo info);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_run_self_test")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineRunSelfTest(nint engine, ref VsaSelfTestReport report);

    [LibraryImport(LibraryName, EntryPoint = "vsa_asset_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult AssetCreate(nint engine, in VsaAssetDesc desc, out nint asset);

    [LibraryImport(LibraryName, EntryPoint = "vsa_asset_get_info")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult AssetGetInfo(nint asset, ref VsaAssetInfo info);

    [LibraryImport(LibraryName, EntryPoint = "vsa_asset_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void AssetRelease(nint asset);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_create")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceCreate(nint engine, in VsaVoiceDesc desc, out ulong voice);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_release")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceRelease(nint engine, ulong voice);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_start")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceStart(nint engine, ulong voice);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_pause")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoicePause(nint engine, ulong voice);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_stop")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceStop(nint engine, ulong voice);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_set_gain")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSetGain(nint engine, ulong voice, float gain);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_set_pitch")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSetPitch(nint engine, ulong voice, float pitch);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_set_looping")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSetLooping(nint engine, ulong voice, uint looping);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_seek")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSeek(nint engine, ulong voice, double positionSeconds);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_fade")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceFade(nint engine, ulong voice, float targetGain, float seconds, uint flags, ulong token);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_get_status")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceGetStatus(nint engine, ulong voice, ref VsaVoiceStatus status);

    [LibraryImport(LibraryName, EntryPoint = "vsa_bus_set_gain")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult BusSetGain(nint engine, uint bus, float gain);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_set_master_gain")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineSetMasterGain(nint engine, float gain);

    [LibraryImport(LibraryName, EntryPoint = "vsa_device_enumerate")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult DeviceEnumerate(nint engine, VsaDeviceInfo* devices, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_output_open")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult OutputOpen(nint engine, in VsaOutputDesc desc);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_render_offline")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineRenderOffline(nint engine, float* output, uint frames);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetStats(nint engine, ref VsaEngineStats stats);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_poll_events")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EnginePollEvents(nint engine, VsaEvent* events, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_get_last_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial byte* GetLastErrorNative();

    /// <summary>The calling thread's last native error message.</summary>
    public static string GetLastError() => Marshal.PtrToStringUTF8((nint)GetLastErrorNative()) ?? string.Empty;
}

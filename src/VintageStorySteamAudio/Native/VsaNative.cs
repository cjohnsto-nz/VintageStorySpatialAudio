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
    /// <summary>
    /// Windows Spatial Audio: the 7.1.4 mix goes to the spatial stream's bed (Dolby Atmos, DTS:X,
    /// Windows Sonic). Falls back to <see cref="Device"/> where unavailable.
    /// </summary>
    Spatial = 2,
}

/// <summary>How a voice is positioned.</summary>
public enum SpatialMode : uint
{
    /// <summary>Not positioned: straight to its bus (music, UI).</summary>
    None = 0,
    /// <summary>World coordinates, rendered relative to the listener.</summary>
    World = 1,
    /// <summary>Listener space (head-locked): +x right, +y up, -z forward.</summary>
    Listener = 2,
}

public enum RenderMode : uint
{
    /// <summary>Binaural (HRTF) for the loudest positional voices, panning for the rest.</summary>
    Headphones = 0,
    /// <summary>Panning to the output's speakers: stereo, quad, 5.1 or 7.1 (unpositioned sounds stay on the front pair).</summary>
    Speakers = 1,
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
    public uint MaxRealVoices;
    public uint MaxBinauralVoices;
    public uint Reserved;
    public byte* HrtfSofaPath;
    public uint OcclusionSamples;
    public uint DirectRateHz;
    public uint ReflectionSources;
    public uint ReflectionRays;
    public uint ReflectionBounces;
    public float ReflectionDuration;
    public uint ReflectionOrder;
    public uint ReflectionRateHz;
    public uint ReflectionThreads;
    public float ReflectionTransition;
    public uint PathingRange;
    public uint PathingHeight;
    public float PathingProbeSpacing;
    public uint PathingVisSamples;
    public uint PathingRateHz;
    public uint PathingSources;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaPathingStats
{
    public uint StructSize;
    public uint Enabled;
    public uint Baking;
    public uint BakeDue;
    public ulong Bakes;
    public double LastBakeMs;
    public double MaxBakeMs;
    public uint Probes;
    public uint Reserved;
    public fixed double BoxCentre[3];
    public ulong Ticks;
    public double LastTickMs;
    public double MaxTickMs;
    public uint Wanted;
    public uint Simulated;
    public uint Found;
    public uint RateHz;
    public fixed float Listener[3];
    public uint Reserved2;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaPathSegment
{
    public uint StructSize;
    public uint Occluded;
    public fixed float From[3];
    public fixed float To[3];
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
    public uint Spatial;
    public float PositionX;
    public float PositionY;
    public float PositionZ;
    public float MinDistance;
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaListener
{
    public uint StructSize;
    public float PositionX;
    public float PositionY;
    public float PositionZ;
    public float ForwardX;
    public float ForwardY;
    public float ForwardZ;
    public float UpX;
    public float UpY;
    public float UpZ;
    public float RenderOffsetX;
    public float RenderOffsetY;
    public float RenderOffsetZ;
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
    public uint RealVoices;
    public uint VirtualVoices;
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

/// <summary>How open a material is to sound: surfaces are meshed where a denser kind meets a more open one.</summary>
public enum MaterialKind : uint
{
    Air = 0,
    Liquid = 1,
    Porous = 2,
    Solid = 3,
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaAcousticMaterial
{
    public uint StructSize;
    public uint Kind;
    public fixed float Absorption[3];
    public float Scattering;
    public fixed float Transmission[3];
    public fixed float AttenuationDbPerMetre[3];
    public byte* Name;
}

[StructLayout(LayoutKind.Sequential)]
public struct VsaBox
{
    public float MinX;
    public float MinY;
    public float MinZ;
    public float MaxX;
    public float MaxY;
    public float MaxZ;

    public VsaBox(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
    {
        MinX = minX;
        MinY = minY;
        MinZ = minZ;
        MaxX = maxX;
        MaxY = maxY;
        MaxZ = maxZ;
    }
}

[StructLayout(LayoutKind.Sequential)]
public struct VsaPartialBlock
{
    public uint Cell;
    public uint Material;
    public uint FirstBox;
    public uint BoxCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaChunkDesc
{
    public uint StructSize;
    public int X;
    public int Y;
    public int Z;
    public uint Lod;
    public uint Reserved;
    public ushort* Materials;
    public VsaPartialBlock* Partials;
    public uint PartialCount;
    public uint BoxCount;
    public VsaBox* Boxes;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaSceneStats
{
    public uint StructSize;
    public uint Chunks;
    public uint MeshedChunks;
    public uint PendingChunks;
    public ulong Triangles;
    public ulong Vertices;
    public ulong MemoryBytes;
    public ulong ChunksBuilt;
    public double LastBuildMs;
    public double MaxBuildMs;
    public double LastCommitMs;
    public fixed int Origin[3];
    public uint MaterialCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaChunkMesh
{
    public uint StructSize;
    public uint Found;
    public uint Lod;
    public uint VertexCount;
    public uint TriangleCount;
    public uint VertexCapacity;
    public uint TriangleCapacity;
    public uint Version;
    public float* Vertices;
    public int* Triangles;
    public ushort* Materials;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaRayHit
{
    public uint StructSize;
    public uint Hit;
    public float Distance;
    public fixed float Point[3];
    public fixed float Normal[3];
    public fixed int Chunk[3];
    public uint Triangle;
    public uint Material;
    public uint FromPartial;
    public fixed int Cell[3];
    public uint Lod;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaSourceDebug
{
    public uint StructSize;
    public uint Flags;
    public ulong Voice;
    public fixed float Position[3];
    public fixed float SimulatedPosition[3];
    public float Occlusion;
    public fixed float Transmission[3];
    public float SolidMetres;
    public uint Crossings;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaThreadStats
{
    public uint StructSize;
    public uint Kind;
    public uint ThreadId;
    public uint Reserved;
    public double CpuMs;
    public fixed byte Name[48];
}

[StructLayout(LayoutKind.Sequential)]
internal struct VsaSimulationStats
{
    public uint StructSize;
    public uint Sources;
    public ulong Ticks;
    public double LastTickMs;
    public double MaxTickMs;
    public double OcclusionMs;
    public double TransmissionMs;
    public uint RateHz;
    public uint OcclusionSamples;
    public float ListenerX;
    public float ListenerY;
    public float ListenerZ;
    public int OriginX;
    public int OriginY;
    public int OriginZ;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaReflectionStats
{
    public uint StructSize;
    public uint Enabled;
    public uint Slots;
    public uint LiveSlots;
    public uint WaitingSlots;
    public uint DrainingSlots;
    public uint Rays;
    public uint Bounces;
    public uint Order;
    public uint RateHz;
    public uint Threads;
    public float Duration;
    public float Transition;
    public uint Reserved;
    public ulong Ticks;
    public double LastTickMs;
    public double MaxTickMs;
    public double SimulateMs;
    public fixed float ListenerReverbTimes[3];
    public float OutputDb;
    public float Gain;
    public fixed float Listener[3];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaReflectionSource
{
    public uint StructSize;
    public uint Slot;
    public ulong Voice;
    public fixed float Position[3];
    public fixed float ReverbTimes[3];
    public fixed float Eq[3];
    public int Delay;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaRaySegment
{
    public uint StructSize;
    public uint Bounce;
    public fixed float From[3];
    public fixed float To[3];
    public float Energy;
    public uint Material;
}

internal static unsafe partial class VsaNative
{
    public const int ChunkSize = 32;
    public const int ChunkCells = ChunkSize * ChunkSize * ChunkSize;

    public const string LibraryName = "vsaudio";

    /// <summary>Must equal VSA_ABI_VERSION in vsaudio.h.</summary>
    public const uint AbiVersion = 12;

    public const uint EngineFlagSteamAudioValidation = 1u << 0;
    public const uint EngineFlagNoDirectSimulation = 1u << 1;
    public const uint EngineFlagNoReflections = 1u << 2;
    public const uint EngineFlagNoPathing = 1u << 3;
    public const uint SourceEscaped = 1u << 0;
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

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_set_position")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSetPosition(nint engine, ulong voice, uint spatial, float x, float y, float z);

    [LibraryImport(LibraryName, EntryPoint = "vsa_voice_set_lowpass")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult VoiceSetLowpass(nint engine, ulong voice, float gainHf);

    [LibraryImport(LibraryName, EntryPoint = "vsa_listener_set")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult ListenerSet(nint engine, in VsaListener listener);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_set_render_mode")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineSetRenderMode(nint engine, uint mode);

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

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_set_materials")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneSetMaterials(nint engine, VsaAcousticMaterial* materials, uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_set_chunk")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneSetChunk(nint engine, in VsaChunkDesc chunk);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_remove_chunk")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneRemoveChunk(nint engine, int x, int y, int z);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_clear")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneClear(nint engine);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_set_origin")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneSetOrigin(nint engine, int x, int y, int z);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_wait_idle")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneWaitIdle(nint engine, uint timeoutMs);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_get_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneGetStats(nint engine, ref VsaSceneStats stats);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_get_chunk_mesh")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneGetChunkMesh(nint engine, int x, int y, int z, ref VsaChunkMesh mesh);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_list_chunks")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneListChunks(nint engine, int* keys, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_raycast")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneRaycast(nint engine, float* origin, float* direction, float maxDistance, ref VsaRayHit hit);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_save_obj")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneSaveObj(nint engine, byte* path);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_sources")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetSources(nint engine, VsaSourceDebug* sources, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_simulation_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetSimulationStats(nint engine, ref VsaSimulationStats stats);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_reflection_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetReflectionStats(nint engine, ref VsaReflectionStats stats);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_reflection_sources")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetReflectionSources(nint engine, VsaReflectionSource* sources, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_set_reflection_gain")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineSetReflectionGain(nint engine, float gain);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_set_reflection_mix")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineSetReflectionMix(nint engine, float earlyGain, float tailGain);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_pathing_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetPathingStats(nint engine, ref VsaPathingStats stats);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_path_segments")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetPathSegments(nint engine, VsaPathSegment* segments, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_engine_get_thread_stats")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult EngineGetThreadStats(nint engine, VsaThreadStats* threads, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_scene_trace_rays")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial VsaResult SceneTraceRays(nint engine, float* origin, uint rays, uint bounces, float maxDistance, VsaRaySegment* segments, uint capacity, out uint count);

    [LibraryImport(LibraryName, EntryPoint = "vsa_get_last_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial byte* GetLastErrorNative();

    /// <summary>The calling thread's last native error message.</summary>
    public static string GetLastError() => Marshal.PtrToStringUTF8((nint)GetLastErrorNative()) ?? string.Empty;
}

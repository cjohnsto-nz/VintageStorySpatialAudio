using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace VintageStorySpatialAudio.Native;

/// <summary>Receives engine log output. Called from engine threads; must be thread-safe.</summary>
public interface IEngineLog
{
    void Write(EngineLogLevel level, string message);
}

public enum EngineLogLevel
{
    Debug,
    Info,
    Warning,
    Error,
}

public sealed record EngineOptions
{
    public RayTracer RayTracer { get; init; } = RayTracer.Auto;

    /// <summary>Steam Audio's API validation layer. Slow; development only.</summary>
    public bool SteamAudioValidation { get; init; }

    /// <summary>Rate of the offline output; devices run at their native rate. 0 = 48000.</summary>
    public int SampleRate { get; init; }

    /// <summary>Frames per engine block. 0 = 256.</summary>
    public int BlockFrames { get; init; }

    /// <summary>Voice slot capacity (a storage bound, not an audibility cap). 0 = 4096.</summary>
    public int MaxVoices { get; init; }

    public ResamplerQuality ResamplerQuality { get; init; } = ResamplerQuality.Default;

    /// <summary>Ogg assets longer than this are streamed. 0 = 20 s.</summary>
    public int StreamThresholdMs { get; init; }

    /// <summary>Positional voices rendered with their own Steam Audio effects at once. 0 = 256.</summary>
    public int MaxRealVoices { get; init; }

    /// <summary>Of those, how many get per-voice HRTF in headphones mode. 0 = 64.</summary>
    public int MaxBinauralVoices { get; init; }

    /// <summary>A SOFA file with the HRTF to use (null = Steam Audio's default). Falls back to the default, with a warning, if it cannot be loaded.</summary>
    public string? HrtfSofaPath { get; init; }

    /// <summary>Occlusion and transmission by the world scene (the direct simulation).</summary>
    public bool DirectSimulation { get; init; } = true;

    /// <summary>Rays per source for volumetric occlusion. 0 = 16.</summary>
    public int OcclusionSamples { get; init; }

    /// <summary>Direct simulation updates per second. 0 = 30.</summary>
    public int DirectRateHz { get; init; }

    /// <summary>Reverb simulated from the world scene (Phase 6).</summary>
    public bool Reflections { get; init; } = true;

    /// <summary>The reflection simulation's quality; zeros are the engine's defaults (Balanced).</summary>
    public ReflectionQualitySettings ReflectionQuality { get; init; } = new();

    /// <summary>Sound round corners and through doorways (Phase 7).</summary>
    public bool Pathing { get; init; } = true;

    /// <summary>The pathing settings; zeros are the engine's defaults.</summary>
    public PathingSettings PathingSettings { get; init; } = new();
}

/// <summary>Pathing settings (vsa_engine_config's pathing_* fields); 0 = the engine's default for each.</summary>
public sealed record PathingSettings
{
    /// <summary>The box baked round the listener, blocks across (32..256).</summary>
    public int RangeBlocks { get; init; }

    /// <summary>The box's height, blocks (16..128).</summary>
    public int HeightBlocks { get; init; }

    /// <summary>Metres between probes (1..8). Widened when the box would hold more than <see cref="MaxProbes"/>.</summary>
    public float ProbeSpacing { get; init; }

    /// <summary>Probes baked at most; the spacing widens to keep within it (64..65536).</summary>
    public int MaxProbes { get; init; }

    /// <summary>Point samples per probe when testing whether two probes see each other (1..8).</summary>
    public int VisibilitySamples { get; init; }

    /// <summary>Simulations per second (1..60).</summary>
    public int RateHz { get; init; }

    /// <summary>Sounds given paths per simulation at most (1..256).</summary>
    public int Sources { get; init; }
}

/// <summary>
/// The reflection simulation's settings (vsa_engine_config's reflection_* fields); 0 = the
/// engine's default for each. <see cref="Config.ReflectionPresets"/> has the named presets.
/// </summary>
public sealed record ReflectionQualitySettings
{
    /// <summary>Places simulated at once (sounds within 3 m of one another share one). 1..64.</summary>
    public int Sources { get; init; }

    /// <summary>Rays traced from the listener per simulation. 256..32768.</summary>
    public int Rays { get; init; }

    /// <summary>Bounces per ray. 1..64.</summary>
    public int Bounces { get; init; }

    /// <summary>Impulse response length, seconds. 0.25..4.</summary>
    public float DurationSeconds { get; init; }

    /// <summary>Ambisonic order of the reflections. 1..3.</summary>
    public int Order { get; init; }

    /// <summary>Simulations per second at most. 1..60.</summary>
    public int RateHz { get; init; }

    /// <summary>Worker threads for one simulation (0 = a quarter of the cores, 1..4).</summary>
    public int Threads { get; init; }

    /// <summary>Seconds of each impulse response convolved (early reflections); the rest is a parametric tail. 0.02..0.5.</summary>
    public float TransitionSeconds { get; init; }
}

/// <summary>Where a positional voice is and how it falls off with distance.</summary>
/// <param name="Mode">World or listener-relative; <see cref="SpatialMode.None"/> for unpositioned voices.</param>
/// <param name="X">Position (world or listener space).</param>
/// <param name="Y">Position (world or listener space).</param>
/// <param name="Z">Position (world or listener space).</param>
/// <param name="MinDistance">Distance within which the source no longer gets louder (0 = 1 m).</param>
/// <param name="OcclusionFloor">
/// The least of the sound that still reaches the listener directly, whatever is in the way
/// (0 = none: the world decides). See vsa_voice_desc::occlusion_floor and ADR 0022.
/// </param>
public readonly record struct VoicePlacement(
    SpatialMode Mode, float X = 0, float Y = 0, float Z = 0, float MinDistance = 0, float OcclusionFloor = 0);

public sealed record EngineVersion(
    Version Engine,
    Version SteamAudio,
    uint AbiVersion,
    string BuildDescription);

public sealed record EngineInfo(RayTracer ActiveRayTracer, bool EmbreeAvailable);

public sealed record SelfTestResult(bool Passed, float OcclusionThroughWall, float OcclusionClearPath, double ElapsedMs);

/// <summary>A playback device. <see cref="Id"/> is opaque and only meaningful to the engine that listed it.</summary>
public sealed record AudioDevice(string Name, bool IsDefault, ReadOnlyMemory<byte> Id);

public sealed record EngineStats(
    OutputKind Output,
    int SampleRate,
    int Channels,
    int BlockFrames,
    int DevicePeriodFrames,
    string DeviceName,
    int ActiveVoices,
    int AllocatedVoices,
    int MaxVoices,
    int RealVoices,
    int VirtualVoices,
    ulong BlocksRendered,
    ulong Overloads,
    ulong StreamUnderruns,
    ulong EventsDropped,
    double RenderTimeAvgUs,
    double RenderTimeMaxUs,
    double BlockPeriodUs,
    float LimiterPeakReductionDb);

public readonly record struct EngineEvent(EngineEventType Type, ulong Voice, ulong Token, bool FadeCancelled);

/// <summary>
/// Owns the native engine. At most one exists per process (enforced natively). Thread-safe: every
/// method may be called from any thread. Dispose it before a new world session creates another.
/// </summary>
public sealed partial class AudioEngine : IDisposable
{
    private readonly EngineHandle handle;
    private int offlineChannels = 2;

    private AudioEngine(EngineHandle handle, EngineInfo info)
    {
        this.handle = handle;
        Info = info;
    }

    public EngineInfo Info { get; }

    public bool IsDisposed => handle.IsClosed;

    /// <summary>Reads the native library's version and checks ABI compatibility.</summary>
    public static EngineVersion GetVersion()
    {
        var info = new VsaVersionInfo { StructSize = (uint)Unsafe.SizeOf<VsaVersionInfo>() };
        NativeException.ThrowIfFailed(VsaNative.GetVersion(ref info), "vsa_get_version");
        return new EngineVersion(
            new Version((int)info.EngineMajor, (int)info.EngineMinor, (int)info.EnginePatch),
            new Version((int)info.SteamAudioMajor, (int)info.SteamAudioMinor, (int)info.SteamAudioPatch),
            info.AbiVersion,
            Marshal.PtrToStringUTF8(info.BuildDescription) ?? string.Empty);
    }

    public static unsafe AudioEngine Create(EngineOptions options, IEngineLog? log)
    {
        ArgumentNullException.ThrowIfNull(options);

        EngineVersion version = GetVersion();
        if (version.AbiVersion != VsaNative.AbiVersion)
        {
            throw new InvalidOperationException(
                $"Native engine ABI {version.AbiVersion} does not match managed ABI {VsaNative.AbiVersion}. " +
                (NativeLibraryResolver.UsingNativePack
                    ? $"The libraries come from the '{NativeLibraryResolver.NativePackModId}' mod, which is from a release with a different ABI; update it."
                    : "The mod package is inconsistent; reinstall it."));
        }

        GCHandle logHandle = log is null ? default : GCHandle.Alloc(log);
        byte[]? sofaPath = string.IsNullOrEmpty(options.HrtfSofaPath) ? null : Encoding.UTF8.GetBytes(options.HrtfSofaPath + "\0");
        try
        {
            nint engine;
            fixed (byte* sofa = sofaPath)
            {
                var config = new VsaEngineConfig
                {
                    StructSize = (uint)sizeof(VsaEngineConfig),
                    AbiVersion = VsaNative.AbiVersion,
                    Log = log is null ? null : &OnNativeLog,
                    LogUserData = log is null ? 0 : GCHandle.ToIntPtr(logHandle),
                    RayTracer = (uint)options.RayTracer,
                    Flags = (options.SteamAudioValidation ? VsaNative.EngineFlagSteamAudioValidation : 0)
                        | (options.DirectSimulation ? 0 : VsaNative.EngineFlagNoDirectSimulation)
                        | (options.Reflections ? 0 : VsaNative.EngineFlagNoReflections)
                        | (options.Pathing ? 0 : VsaNative.EngineFlagNoPathing),
                    SampleRate = checked((uint)options.SampleRate),
                    BlockFrames = checked((uint)options.BlockFrames),
                    MaxVoices = checked((uint)options.MaxVoices),
                    ResamplerQuality = (uint)options.ResamplerQuality,
                    StreamThresholdMs = checked((uint)options.StreamThresholdMs),
                    MaxRealVoices = checked((uint)options.MaxRealVoices),
                    MaxBinauralVoices = checked((uint)options.MaxBinauralVoices),
                    HrtfSofaPath = sofa,
                    OcclusionSamples = checked((uint)options.OcclusionSamples),
                    DirectRateHz = checked((uint)options.DirectRateHz),
                    ReflectionSources = checked((uint)options.ReflectionQuality.Sources),
                    ReflectionRays = checked((uint)options.ReflectionQuality.Rays),
                    ReflectionBounces = checked((uint)options.ReflectionQuality.Bounces),
                    ReflectionDuration = options.ReflectionQuality.DurationSeconds,
                    ReflectionOrder = checked((uint)options.ReflectionQuality.Order),
                    ReflectionRateHz = checked((uint)options.ReflectionQuality.RateHz),
                    ReflectionThreads = checked((uint)options.ReflectionQuality.Threads),
                    ReflectionTransition = options.ReflectionQuality.TransitionSeconds,
                    PathingRange = checked((uint)options.PathingSettings.RangeBlocks),
                    PathingHeight = checked((uint)options.PathingSettings.HeightBlocks),
                    PathingProbeSpacing = options.PathingSettings.ProbeSpacing,
                    PathingVisSamples = checked((uint)options.PathingSettings.VisibilitySamples),
                    PathingRateHz = checked((uint)options.PathingSettings.RateHz),
                    PathingSources = checked((uint)options.PathingSettings.Sources),
                    PathingMaxProbes = checked((uint)options.PathingSettings.MaxProbes),
                };

                NativeException.ThrowIfFailed(VsaNative.EngineCreate(in config, out engine), "vsa_engine_create");
            }

            var engineHandle = new EngineHandle(engine, logHandle);
            logHandle = default; // now owned by engineHandle

            var info = new VsaEngineInfo { StructSize = (uint)sizeof(VsaEngineInfo) };
            VsaResult result = VsaNative.EngineGetInfo(engine, ref info);
            if (result != VsaResult.Ok)
            {
                string message = VsaNative.GetLastError();
                engineHandle.Dispose();
                throw new NativeException("vsa_engine_get_info", result, message);
            }

            return new AudioEngine(engineHandle, new EngineInfo((RayTracer)info.ActiveRayTracer, info.EmbreeAvailable != 0));
        }
        finally
        {
            if (logHandle.IsAllocated)
            {
                logHandle.Free();
            }
        }
    }

    /// <summary>Runs the native end-to-end self-test (scene, ray tracer, direct simulation).</summary>
    public SelfTestResult RunSelfTest()
    {
        using Lease lease = new(handle);
        var report = new VsaSelfTestReport { StructSize = (uint)Unsafe.SizeOf<VsaSelfTestReport>() };
        NativeException.ThrowIfFailed(VsaNative.EngineRunSelfTest(lease.Engine, ref report), "vsa_engine_run_self_test");
        return new SelfTestResult(report.Passed != 0, report.OcclusionThroughWall, report.OcclusionClearPath, report.ElapsedMs);
    }

    // ---- assets and voices ----

    /// <summary>Decodes (or, for streamed storage, validates and copies) audio on the calling thread.</summary>
    public unsafe AudioAsset CreateAsset(
        ReadOnlySpan<byte> data, string? name = null, AssetFormat format = AssetFormat.Auto, AssetStorage storage = AssetStorage.Auto)
    {
        byte[]? nameBytes = name is null ? null : Encoding.UTF8.GetBytes(name + "\0");
        using Lease lease = new(handle);
        fixed (byte* bytes = data)
        fixed (byte* namePtr = nameBytes)
        {
            var desc = new VsaAssetDesc
            {
                StructSize = (uint)sizeof(VsaAssetDesc),
                Format = (uint)format,
                Storage = (uint)storage,
                Data = bytes,
                Size = (ulong)data.Length,
                Name = namePtr,
            };
            NativeException.ThrowIfFailed(VsaNative.AssetCreate(lease.Engine, in desc, out nint asset), "vsa_asset_create");
            return new AudioAsset(asset);
        }
    }

    /// <summary>Wraps interleaved 16-bit PCM.</summary>
    public unsafe AudioAsset CreatePcmAsset(ReadOnlySpan<short> interleaved, int channels, int sampleRate, string? name = null)
    {
        byte[]? nameBytes = name is null ? null : Encoding.UTF8.GetBytes(name + "\0");
        using Lease lease = new(handle);
        fixed (short* samples = interleaved)
        fixed (byte* namePtr = nameBytes)
        {
            var desc = new VsaAssetDesc
            {
                StructSize = (uint)sizeof(VsaAssetDesc),
                Format = (uint)AssetFormat.PcmS16,
                Data = samples,
                Size = (ulong)interleaved.Length * sizeof(short),
                Name = namePtr,
                PcmChannels = checked((uint)channels),
                PcmSampleRate = checked((uint)sampleRate),
            };
            NativeException.ThrowIfFailed(VsaNative.AssetCreate(lease.Engine, in desc, out nint asset), "vsa_asset_create");
            return new AudioAsset(asset);
        }
    }

    /// <summary>Creates a stopped voice. The voice keeps the asset alive; the caller may dispose its asset.</summary>
    public Voice CreateVoice(
        AudioAsset asset, AudioBus bus = AudioBus.Sound, float gain = 1f, float pitch = 1f, bool looping = false, VoicePlacement placement = default)
    {
        ArgumentNullException.ThrowIfNull(asset);
        using Lease lease = new(handle);
        using AudioAsset.Lease assetLease = asset.Acquire();
        var desc = new VsaVoiceDesc
        {
            StructSize = (uint)Unsafe.SizeOf<VsaVoiceDesc>(),
            Bus = (uint)bus,
            Asset = assetLease.Handle,
            Gain = gain,
            Pitch = pitch,
            Looping = looping ? 1u : 0u,
            Spatial = (uint)placement.Mode,
            PositionX = placement.X,
            PositionY = placement.Y,
            PositionZ = placement.Z,
            MinDistance = placement.MinDistance,
            OcclusionFloor = placement.OcclusionFloor,
        };
        NativeException.ThrowIfFailed(VsaNative.VoiceCreate(lease.Engine, in desc, out ulong voice), "vsa_voice_create");
        return new Voice(this, voice);
    }

    public void SetBusGain(AudioBus bus, float gain)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.BusSetGain(lease.Engine, (uint)bus, gain), "vsa_bus_set_gain");
    }

    public void SetMasterGain(float gain)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.EngineSetMasterGain(lease.Engine, gain), "vsa_engine_set_master_gain");
    }

    /// <summary>Listener pose for positional voices: position and unit forward/up vectors.</summary>
    /// <summary>
    /// The listener. The render offset is added to the position for rendering only (panning and
    /// spatialisation), not for the simulation, which listens from the position itself.
    /// </summary>
    public void SetListener(
        float x, float y, float z, float forwardX, float forwardY, float forwardZ, float upX, float upY, float upZ,
        float offsetX = 0f, float offsetY = 0f, float offsetZ = 0f)
    {
        using Lease lease = new(handle);
        var listener = new VsaListener
        {
            StructSize = (uint)Unsafe.SizeOf<VsaListener>(),
            PositionX = x,
            PositionY = y,
            PositionZ = z,
            ForwardX = forwardX,
            ForwardY = forwardY,
            ForwardZ = forwardZ,
            UpX = upX,
            UpY = upY,
            UpZ = upZ,
            RenderOffsetX = offsetX,
            RenderOffsetY = offsetY,
            RenderOffsetZ = offsetZ,
        };
        NativeException.ThrowIfFailed(VsaNative.ListenerSet(lease.Engine, in listener), "vsa_listener_set");
    }

    public void SetRenderMode(RenderMode mode)
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.EngineSetRenderMode(lease.Engine, (uint)mode), "vsa_engine_set_render_mode");
    }

    // ---- output ----

    public unsafe IReadOnlyList<AudioDevice> EnumerateDevices()
    {
        using Lease lease = new(handle);
        NativeException.ThrowIfFailed(VsaNative.DeviceEnumerate(lease.Engine, null, 0, out uint count), "vsa_device_enumerate");
        var records = new VsaDeviceInfo[count + 4]; // headroom for a device appearing in between
        records[0].StructSize = (uint)sizeof(VsaDeviceInfo);
        fixed (VsaDeviceInfo* ptr = records)
        {
            NativeException.ThrowIfFailed(
                VsaNative.DeviceEnumerate(lease.Engine, ptr, (uint)records.Length, out count), "vsa_device_enumerate");
            int written = (int)Math.Min(count, (uint)records.Length);
            var devices = new List<AudioDevice>(written);
            for (int i = 0; i < written; i++)
            {
                VsaDeviceInfo* record = ptr + i;
                string name = Utf8(record->Name, VsaDeviceInfo.NameLength);
                byte[] id = new ReadOnlySpan<byte>(record->Id.Bytes, VsaDeviceId.Length).ToArray();
                devices.Add(new AudioDevice(name, record->IsDefault != 0, id));
            }

            return devices;
        }
    }

    /// <summary>
    /// Opens a device (null = the system default, followed when it changes). With
    /// <paramref name="spatial"/>, through Windows Spatial Audio (a 7.1.4 bed) when the device has
    /// a spatial sound format enabled, otherwise directly; <see cref="EngineStats.Output"/> says which.
    /// </summary>
    public unsafe void OpenDevice(AudioDevice? device = null, int channels = 0, bool spatial = false)
    {
        using Lease lease = new(handle);
        VsaDeviceId id = default;
        if (device is not null)
        {
            if (device.Id.Length != VsaDeviceId.Length)
            {
                throw new ArgumentException("Device id has the wrong length; it must come from EnumerateDevices.", nameof(device));
            }

            device.Id.Span.CopyTo(new Span<byte>(id.Bytes, VsaDeviceId.Length));
        }

        var desc = new VsaOutputDesc
        {
            StructSize = (uint)sizeof(VsaOutputDesc),
            Kind = (uint)(spatial ? OutputKind.Spatial : OutputKind.Device),
            DeviceId = device is null ? null : &id,
            Channels = checked((uint)channels),
        };
        NativeException.ThrowIfFailed(VsaNative.OutputOpen(lease.Engine, in desc), "vsa_output_open");
    }

    /// <summary>Switches to the offline output (audio only through <see cref="RenderOffline"/>).</summary>
    public unsafe void OpenOffline(int sampleRate = 0, int channels = 2)
    {
        using Lease lease = new(handle);
        var desc = new VsaOutputDesc
        {
            StructSize = (uint)sizeof(VsaOutputDesc),
            Kind = (uint)OutputKind.None,
            Channels = checked((uint)channels),
            SampleRate = checked((uint)sampleRate),
        };
        NativeException.ThrowIfFailed(VsaNative.OutputOpen(lease.Engine, in desc), "vsa_output_open");
        offlineChannels = channels == 0 ? 2 : channels;
    }

    /// <summary>Renders interleaved frames on the calling thread (offline output only). Deterministic.</summary>
    public unsafe void RenderOffline(Span<float> interleaved)
    {
        int channels = offlineChannels;
        if (interleaved.Length % channels != 0)
        {
            throw new ArgumentException($"Length must be a multiple of the channel count ({channels}).", nameof(interleaved));
        }

        using Lease lease = new(handle);
        fixed (float* output = interleaved)
        {
            NativeException.ThrowIfFailed(
                VsaNative.EngineRenderOffline(lease.Engine, output, (uint)(interleaved.Length / channels)), "vsa_engine_render_offline");
        }
    }

    /// <summary>Engine telemetry. Render times and limiter reduction cover the time since the previous call.</summary>
    public unsafe EngineStats GetStats()
    {
        using Lease lease = new(handle);
        var stats = new VsaEngineStats { StructSize = (uint)sizeof(VsaEngineStats) };
        NativeException.ThrowIfFailed(VsaNative.EngineGetStats(lease.Engine, ref stats), "vsa_engine_get_stats");
        return new EngineStats(
            (OutputKind)stats.OutputKind,
            (int)stats.SampleRate,
            (int)stats.Channels,
            (int)stats.BlockFrames,
            (int)stats.DevicePeriodFrames,
            Utf8(stats.DeviceName, 256),
            (int)stats.ActiveVoices,
            (int)stats.AllocatedVoices,
            (int)stats.MaxVoices,
            (int)stats.RealVoices,
            (int)stats.VirtualVoices,
            stats.BlocksRendered,
            stats.Overloads,
            stats.StreamUnderruns,
            stats.EventsDropped,
            stats.RenderTimeAvgUs,
            stats.RenderTimeMaxUs,
            stats.BlockPeriodUs,
            stats.LimiterPeakReductionDb);
    }

    /// <summary>Takes up to <paramref name="buffer"/>.Length pending events; returns how many were written.</summary>
    public unsafe int PollEvents(Span<EngineEvent> buffer)
    {
        if (buffer.IsEmpty)
        {
            return 0;
        }

        using Lease lease = new(handle);
        Span<VsaEvent> raw = buffer.Length <= 64 ? stackalloc VsaEvent[buffer.Length] : new VsaEvent[buffer.Length];
        raw[0].StructSize = (uint)sizeof(VsaEvent);
        uint count;
        fixed (VsaEvent* ptr = raw)
        {
            NativeException.ThrowIfFailed(VsaNative.EnginePollEvents(lease.Engine, ptr, (uint)raw.Length, out count), "vsa_engine_poll_events");
        }

        for (int i = 0; i < count; i++)
        {
            buffer[i] = new EngineEvent(
                (EngineEventType)raw[i].Type, raw[i].Voice, raw[i].Token, (raw[i].Flags & VsaNative.EventFlagFadeCancelled) != 0);
        }

        return (int)count;
    }

    public void Dispose() => handle.Dispose();

    internal Lease Acquire() => new(handle);

    private static unsafe string Utf8(byte* text, int capacity)
    {
        var span = new ReadOnlySpan<byte>(text, capacity);
        int end = span.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? span : span[..end]);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe void OnNativeLog(nint userData, VsaLogLevel level, byte* message)
    {
        // Must never throw back into native code.
        try
        {
            if (userData == 0 || GCHandle.FromIntPtr(userData).Target is not IEngineLog log)
            {
                return;
            }

            string text = Marshal.PtrToStringUTF8((nint)message) ?? string.Empty;
            EngineLogLevel mapped = level switch
            {
                VsaLogLevel.Debug => EngineLogLevel.Debug,
                VsaLogLevel.Warning => EngineLogLevel.Warning,
                VsaLogLevel.Error => EngineLogLevel.Error,
                _ => EngineLogLevel.Info,
            };
            log.Write(mapped, text);
        }
        catch
        {
            // Swallow: logging must not be able to crash the engine.
        }
    }

    /// <summary>Keeps the native engine alive for the duration of one call (throws once disposed).</summary>
    internal readonly ref struct Lease
    {
        private readonly SafeHandle owner;
        private readonly bool added;

        public Lease(SafeHandle owner)
        {
            this.owner = owner;
            bool ok = false;
            try
            {
                owner.DangerousAddRef(ref ok);
            }
            catch (ObjectDisposedException)
            {
                throw new ObjectDisposedException(nameof(AudioEngine));
            }

            added = ok;
            Engine = owner.DangerousGetHandle();
        }

        public nint Engine { get; }

        public void Dispose()
        {
            if (added)
            {
                owner.DangerousRelease();
            }
        }
    }

    private sealed class EngineHandle : SafeHandle
    {
        private GCHandle logHandle;

        public EngineHandle(nint engine, GCHandle logHandle)
            : base(0, ownsHandle: true)
        {
            SetHandle(engine);
            this.logHandle = logHandle;
        }

        public override bool IsInvalid => handle == 0;

        protected override bool ReleaseHandle()
        {
            // Destroy first: it detaches the native log sink, after which the callback
            // can no longer observe the GCHandle.
            VsaNative.EngineDestroy(handle);
            if (logHandle.IsAllocated)
            {
                logHandle.Free();
            }

            return true;
        }
    }
}

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace VintageStorySteamAudio.Native;

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
}

public sealed record EngineVersion(
    Version Engine,
    Version SteamAudio,
    uint AbiVersion,
    string BuildDescription);

public sealed record EngineInfo(RayTracer ActiveRayTracer, bool EmbreeAvailable);

public sealed record SelfTestResult(bool Passed, float OcclusionThroughWall, float OcclusionClearPath, double ElapsedMs);

/// <summary>
/// Owns the native engine. At most one exists per process (enforced natively).
/// Dispose it before a new world session creates another.
/// </summary>
public sealed class AudioEngine : IDisposable
{
    private readonly EngineHandle handle;

    private AudioEngine(EngineHandle handle, EngineInfo info)
    {
        this.handle = handle;
        Info = info;
    }

    public EngineInfo Info { get; }

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
                "The mod package is inconsistent; reinstall it.");
        }

        GCHandle logHandle = log is null ? default : GCHandle.Alloc(log);
        try
        {
            var config = new VsaEngineConfig
            {
                StructSize = (uint)sizeof(VsaEngineConfig),
                AbiVersion = VsaNative.AbiVersion,
                Log = log is null ? null : &OnNativeLog,
                LogUserData = log is null ? 0 : GCHandle.ToIntPtr(logHandle),
                RayTracer = (uint)options.RayTracer,
                Flags = options.SteamAudioValidation ? VsaNative.EngineFlagSteamAudioValidation : 0,
            };

            NativeException.ThrowIfFailed(VsaNative.EngineCreate(in config, out nint engine), "vsa_engine_create");
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
        bool added = false;
        handle.DangerousAddRef(ref added);
        try
        {
            var report = new VsaSelfTestReport { StructSize = (uint)Unsafe.SizeOf<VsaSelfTestReport>() };
            NativeException.ThrowIfFailed(
                VsaNative.EngineRunSelfTest(handle.DangerousGetHandle(), ref report), "vsa_engine_run_self_test");
            return new SelfTestResult(report.Passed != 0, report.OcclusionThroughWall, report.OcclusionClearPath, report.ElapsedMs);
        }
        finally
        {
            if (added)
            {
                handle.DangerousRelease();
            }
        }
    }

    public void Dispose() => handle.Dispose();

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

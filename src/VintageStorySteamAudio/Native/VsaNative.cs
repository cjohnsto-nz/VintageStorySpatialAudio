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

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct VsaEngineConfig
{
    public uint StructSize;
    public uint AbiVersion;
    public delegate* unmanaged[Cdecl]<nint, VsaLogLevel, byte*, void> Log;
    public nint LogUserData;
    public uint RayTracer;
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
    public const uint AbiVersion = 1;

    public const uint EngineFlagSteamAudioValidation = 1u << 0;

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

    [LibraryImport(LibraryName, EntryPoint = "vsa_get_last_error")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial byte* GetLastErrorNative();

    /// <summary>The calling thread's last native error message.</summary>
    public static string GetLastError() => Marshal.PtrToStringUTF8((nint)GetLastErrorNative()) ?? string.Empty;
}

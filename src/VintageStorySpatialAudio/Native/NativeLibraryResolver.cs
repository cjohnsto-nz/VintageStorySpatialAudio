using System.Reflection;
using System.Runtime.InteropServices;

namespace VintageStorySpatialAudio.Native;

/// <summary>
/// Resolves the engine's native libraries from the mod's own <c>native/&lt;rid&gt;/</c> folder, or
/// from the native pack's (<see cref="NativePackModId"/>: Linux and macOS, a second download).
/// </summary>
/// <remarks>
/// Vintage Story extracts zip mods to <c>Cache/unpack/&lt;mod&gt;_&lt;hash&gt;/</c> and loads the
/// mod assembly from there, so the native folder sits next to <see cref="Assembly.Location"/>.
/// <c>phonon</c> is loaded by absolute path before <c>vsaudio</c>: on Windows this satisfies
/// vsaudio.dll's import of phonon.dll (the loader matches already-loaded modules by name);
/// on Linux and macOS vsaudio also finds it through its <c>$ORIGIN</c>/<c>@loader_path</c> rpath.
/// Nothing is added to PATH and no process-wide search paths change.
/// </remarks>
internal static class NativeLibraryResolver
{
    private static readonly Lock Gate = new();
    private static string? registeredDirectory;
    private static nint vsaudioHandle;

    /// <summary>Folder name under <c>native/</c> for the current process.</summary>
    public static string RuntimeFolderName
    {
        get
        {
            Architecture arch = RuntimeInformation.ProcessArchitecture;
            if (OperatingSystem.IsWindows() && arch == Architecture.X64) return "win-x64";
            if (OperatingSystem.IsLinux() && arch == Architecture.X64) return "linux-x64";
            if (OperatingSystem.IsMacOS() && (arch == Architecture.Arm64 || arch == Architecture.X64)) return "osx";
            throw new PlatformNotSupportedException(
                $"Spatial Audio for Vintage Story does not support {RuntimeInformation.OSDescription} ({arch}).");
        }
    }

    /// <summary>Platform file name for a native library base name, e.g. vsaudio → libvsaudio.so.</summary>
    public static string PlatformFileName(string baseName)
    {
        if (OperatingSystem.IsWindows()) return baseName + ".dll";
        if (OperatingSystem.IsMacOS()) return "lib" + baseName + ".dylib";
        return "lib" + baseName + ".so";
    }

    /// <summary>The native folder for a mod assembly located at <paramref name="assemblyLocation"/>.</summary>
    public static string DefaultNativeDirectory(string assemblyLocation)
    {
        string? modDirectory = Path.GetDirectoryName(assemblyLocation);
        if (string.IsNullOrEmpty(modDirectory))
        {
            throw new InvalidOperationException(
                "The mod assembly has no file location, so its native folder cannot be found. " +
                "Install the mod as a zip or folder in VintagestoryData/Mods.");
        }

        return Path.Combine(modDirectory, "native", RuntimeFolderName);
    }

    /// <summary>The mod that carries the Linux and macOS libraries (the main download has Windows').</summary>
    public const string NativePackModId = "spatialaudiounix";

    /// <summary>That mod's assembly, beside which its <c>native/</c> folder is unpacked.</summary>
    public const string NativePackAssemblyName = "SpatialAudioUnixNatives";

    /// <summary>
    /// The first version of the mod that loads a native pack of any version (ADR 0021). Before it
    /// the pack had to be the mod's own version, so the pack declares this as its minimum.
    /// </summary>
    public const string FirstVersionAcceptingAnyPack = "0.1.1";

    /// <summary>Whether this platform's libraries came from the native pack rather than the mod's own folder.</summary>
    public static bool UsingNativePack { get; private set; }

    /// <summary>
    /// The folder holding this platform's libraries: the mod's own, or else the native pack's.
    /// The pack's version is not looked at (ADR 0021). It carries libraries and no code of ours,
    /// so what has to agree is the ABI, which <see cref="AudioEngine.Create"/> checks against the
    /// library itself; the mod's version says nothing about that.
    /// </summary>
    /// <param name="assemblyLocation">Where the mod's assembly is.</param>
    /// <param name="packAssemblyLocation">Where the native pack's assembly is; null: not installed.</param>
    public static string FindNativeDirectory(string assemblyLocation, string? packAssemblyLocation)
    {
        string own = DefaultNativeDirectory(assemblyLocation);
        string library = PlatformFileName(VsaNative.LibraryName);
        if (File.Exists(Path.Combine(own, library)))
        {
            UsingNativePack = false;
            return own;
        }

        if (!string.IsNullOrEmpty(packAssemblyLocation))
        {
            string pack = DefaultNativeDirectory(packAssemblyLocation);
            if (File.Exists(Path.Combine(pack, library)))
            {
                UsingNativePack = true;
                return pack;
            }
        }

        throw new DllNotFoundException(OperatingSystem.IsWindows()
            ? $"The mod's libraries for this platform ({RuntimeFolderName}) are missing from '{own}'. Download the mod again."
            : $"The libraries for {RuntimeFolderName} come in a second mod, because of the mod database's size limit: install "
              + $"'Spatial Audio: Linux and macOS natives' ({NativePackModId}).");
    }

    /// <summary>The native pack's assembly location if the pack is loaded in this process; else null.</summary>
    public static string? LoadedNativePackLocation()
    {
        foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (string.Equals(assembly.GetName().Name, NativePackAssemblyName, StringComparison.Ordinal) && !string.IsNullOrEmpty(assembly.Location))
            {
                return assembly.Location;
            }
        }

        return null;
    }

    /// <summary>
    /// Registers the resolver for this assembly. Safe to call once per world session: the
    /// assembly (and therefore the resolver) survives between sessions, so repeated calls
    /// with the same directory are no-ops.
    /// </summary>
    public static void Register(string nativeDirectory)
    {
        string fullPath = Path.GetFullPath(nativeDirectory);
        lock (Gate)
        {
            if (registeredDirectory is not null)
            {
                if (!string.Equals(registeredDirectory, fullPath, StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(
                        $"Native resolver already registered for '{registeredDirectory}', cannot switch to '{fullPath}'.");
                }

                return;
            }

            NativeLibrary.SetDllImportResolver(typeof(NativeLibraryResolver).Assembly, Resolve);
            registeredDirectory = fullPath;
        }
    }

    private static nint Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, VsaNative.LibraryName, StringComparison.Ordinal))
        {
            return 0;
        }

        lock (Gate)
        {
            if (vsaudioHandle != 0)
            {
                return vsaudioHandle;
            }

            string directory = registeredDirectory
                ?? throw new InvalidOperationException("NativeLibraryResolver.Register was not called.");

            string phonon = Path.Combine(directory, PlatformFileName("phonon"));
            string vsaudio = Path.Combine(directory, PlatformFileName(VsaNative.LibraryName));
            foreach (string required in new[] { phonon, vsaudio })
            {
                if (!File.Exists(required))
                {
                    throw new DllNotFoundException(
                        $"Missing native library '{required}'. The mod package is incomplete for this platform ({RuntimeFolderName}).");
                }
            }

            // Intentionally never freed: the engine lives for the process once loaded.
            NativeLibrary.Load(phonon);
            vsaudioHandle = NativeLibrary.Load(vsaudio);
            return vsaudioHandle;
        }
    }
}

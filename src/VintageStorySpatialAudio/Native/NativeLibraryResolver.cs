using System.Reflection;
using System.Runtime.InteropServices;

namespace VintageStorySpatialAudio.Native;

/// <summary>
/// Resolves the engine's native libraries from the mod's own <c>native/&lt;rid&gt;/</c> folder.
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

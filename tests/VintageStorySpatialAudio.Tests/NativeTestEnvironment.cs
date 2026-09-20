using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// Tests that create a native engine run one at a time: the engine is a per-process singleton.
/// </summary>
[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class NativeEngineGroup
{
    public const string Name = "Native engine";
}

/// <summary>Locates the native build (artifacts/native/&lt;rid&gt;) the tests run against.</summary>
internal static class NativeTestEnvironment
{
    public static string NativeDirectory =>
        Path.Combine(RepoRoot(), "artifacts", "native", NativeLibraryResolver.RuntimeFolderName);

    public static bool IsBuilt => File.Exists(Path.Combine(NativeDirectory, NativeLibraryResolver.PlatformFileName(VsaNative.LibraryName)));

    /// <summary>Registers the resolver, or skips the calling test when the natives are not built.</summary>
    public static void RequireNatives()
    {
        if (!IsBuilt)
        {
            Assert.Skip($"native engine not built at {NativeDirectory}");
        }

        NativeLibraryResolver.Register(NativeDirectory);
    }

    public static string RepoRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "global.json")))
        {
            directory = directory.Parent;
        }

        return directory?.FullName ?? throw new InvalidOperationException("repo root not found");
    }
}

using System.Text.Json;
using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// The Linux and macOS libraries are a second mod (the mod database's 40 MB limit): the main mod
/// takes its own libraries first, then the pack's, and only a pack of its own version.
/// </summary>
public sealed class NativePackTests : IDisposable
{
    private readonly string root = Path.Combine(Path.GetTempPath(), "spatialaudio-pack-" + Guid.NewGuid().ToString("N"));

    public void Dispose()
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    /// <summary>An unpacked mod folder; returns where its assembly would be.</summary>
    private string Mod(string name, bool withNatives)
    {
        string folder = Path.Combine(root, name);
        Directory.CreateDirectory(folder);
        if (withNatives)
        {
            string native = Path.Combine(folder, "native", NativeLibraryResolver.RuntimeFolderName);
            Directory.CreateDirectory(native);
            File.WriteAllText(Path.Combine(native, NativeLibraryResolver.PlatformFileName(VsaNative.LibraryName)), string.Empty);
        }

        return Path.Combine(folder, name + ".dll");
    }

    [Fact]
    public void The_mods_own_libraries_come_first()
    {
        string own = Mod("main", withNatives: true);
        string pack = Mod("pack", withNatives: true);
        Assert.Equal(NativeLibraryResolver.DefaultNativeDirectory(own), NativeLibraryResolver.FindNativeDirectory(own, pack, "1.0.0", "0.9.0"));
    }

    [Fact]
    public void Without_its_own_the_packs_are_used_if_the_versions_agree()
    {
        string own = Mod("main", withNatives: false);
        string pack = Mod("pack", withNatives: true);
        Assert.Equal(NativeLibraryResolver.DefaultNativeDirectory(pack), NativeLibraryResolver.FindNativeDirectory(own, pack, "1.0.0", "1.0.0"));
        var error = Assert.Throws<InvalidOperationException>(() => NativeLibraryResolver.FindNativeDirectory(own, pack, "1.0.1", "1.0.0"));
        Assert.Contains("same version", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void With_neither_the_error_says_what_to_install()
    {
        string own = Mod("main", withNatives: false);
        var error = Assert.Throws<DllNotFoundException>(() => NativeLibraryResolver.FindNativeDirectory(own, null, "1.0.0", null));
        Assert.Contains(OperatingSystem.IsWindows() ? "Download the mod again" : NativeLibraryResolver.NativePackModId, error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void The_pack_is_the_main_mods_version_and_depends_on_exactly_it()
    {
        string src = Path.Combine(NativeTestEnvironment.RepoRoot(), "src");
        using JsonDocument main = Read(Path.Combine(src, "VintageStorySpatialAudio", "modinfo.json"));
        using JsonDocument pack = Read(Path.Combine(src, "SpatialAudioUnixNatives", "modinfo.json"));
        string version = main.RootElement.GetProperty("version").GetString()!;
        Assert.Equal(NativeLibraryResolver.NativePackModId, pack.RootElement.GetProperty("modid").GetString());
        Assert.Equal(version, pack.RootElement.GetProperty("version").GetString());
        Assert.Equal(version, pack.RootElement.GetProperty("dependencies").GetProperty(main.RootElement.GetProperty("modid").GetString()!).GetString());
    }

    private static JsonDocument Read(string path) =>
        JsonDocument.Parse(File.ReadAllText(path), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
}

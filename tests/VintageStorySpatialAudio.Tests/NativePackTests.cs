using System.Text.Json;
using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// The Linux and macOS libraries are a second mod (the mod database's 40 MB limit): the main mod
/// takes its own libraries first, then the pack's, whatever version the pack is (ADR 0021).
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
        Assert.Equal(NativeLibraryResolver.DefaultNativeDirectory(own), NativeLibraryResolver.FindNativeDirectory(own, pack));
        Assert.False(NativeLibraryResolver.UsingNativePack);
    }

    [Fact]
    public void Without_its_own_the_packs_are_used_whatever_version_it_is()
    {
        // The pack is libraries and no code of ours, so its mod version says nothing about whether
        // it fits: the ABI check in AudioEngine.Create is what decides (ADR 0021).
        string own = Mod("main", withNatives: false);
        string pack = Mod("pack", withNatives: true);
        Assert.Equal(NativeLibraryResolver.DefaultNativeDirectory(pack), NativeLibraryResolver.FindNativeDirectory(own, pack));
        Assert.True(NativeLibraryResolver.UsingNativePack);
    }

    [Fact]
    public void With_neither_the_error_says_what_to_install()
    {
        string own = Mod("main", withNatives: false);
        var error = Assert.Throws<DllNotFoundException>(() => NativeLibraryResolver.FindNativeDirectory(own, null));
        Assert.Contains(OperatingSystem.IsWindows() ? "Download the mod again" : NativeLibraryResolver.NativePackModId, error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void The_pack_asks_for_a_mod_new_enough_to_load_it_and_no_newer()
    {
        // The pack's dependency is a minimum (Vintage Story's ModDependency.Version), so the pack
        // may lag the mod: it is only rebuilt when the libraries change (ADR 0021).
        string src = Path.Combine(NativeTestEnvironment.RepoRoot(), "src");
        using JsonDocument main = Read(Path.Combine(src, "VintageStorySpatialAudio", "modinfo.json"));
        using JsonDocument pack = Read(Path.Combine(src, "SpatialAudioUnixNatives", "modinfo.json"));
        Version version = Version.Parse(main.RootElement.GetProperty("version").GetString()!);
        Assert.Equal(NativeLibraryResolver.NativePackModId, pack.RootElement.GetProperty("modid").GetString());

        Version packVersion = Version.Parse(pack.RootElement.GetProperty("version").GetString()!);
        Version floor = Version.Parse(
            pack.RootElement.GetProperty("dependencies").GetProperty(main.RootElement.GetProperty("modid").GetString()!).GetString()!);

        // Neither may claim to be newer than the mod they are built beside.
        Assert.True(floor <= version, $"the pack asks for {floor}, newer than the mod's {version}");
        Assert.True(packVersion <= version, $"the pack is {packVersion}, newer than the mod's {version}");

        // Every mod from the floor upwards must actually load this pack. From
        // FirstVersionAcceptingAnyPack on, all of them do; before it a mod took only a pack of its
        // own version, which is satisfied when the floor is the pack's own version.
        Assert.True(
            floor >= Version.Parse(NativeLibraryResolver.FirstVersionAcceptingAnyPack) || floor == packVersion,
            $"a mod at the floor {floor} would refuse a {packVersion} pack");
    }

    private static JsonDocument Read(string path) =>
        JsonDocument.Parse(File.ReadAllText(path), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
}

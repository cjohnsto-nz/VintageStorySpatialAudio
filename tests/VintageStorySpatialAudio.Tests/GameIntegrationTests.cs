using System.Reflection;
using System.Runtime.Loader;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Platform;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// Tests against the real game install (VINTAGE_STORY) and the real native build
/// (artifacts/native). They are skipped, not failed, when either is absent, so the
/// pure unit tests still run anywhere; CI provides both.
/// </summary>
[Collection(NativeEngineGroup.Name)]
public sealed class GameIntegrationTests
{
    private static readonly Lazy<GameAssemblies?> GameInstall = new(LoadGame);

    private static GameAssemblies? LoadGame()
    {
        string? directory = Environment.GetEnvironmentVariable("VINTAGE_STORY");
        if (directory is null || !File.Exists(Path.Combine(directory, "VintagestoryLib.dll")))
        {
            return null;
        }

        string[] probe = [directory, Path.Combine(directory, "Lib"), Path.Combine(directory, "Mods")];
        AssemblyLoadContext.Default.Resolving += (context, name) =>
            probe.Select(d => Path.Combine(d, name.Name + ".dll")).Where(File.Exists).Select(context.LoadFromAssemblyPath).FirstOrDefault();

        Assembly lib = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(directory, "VintagestoryLib.dll"));
        Assembly api = typeof(Vintagestory.API.Common.ModSystem).Assembly;
        return new GameAssemblies(lib, api);
    }

    [Fact]
    public void Every_audio_integration_point_exists_in_the_installed_game()
    {
        GameAssemblies? game = GameInstall.Value;
        if (game is null)
        {
            Assert.Skip("VINTAGE_STORY is not set to a game install");
        }

        VerificationReport report = PatchTargetVerifier.Verify(game, AudioPatchTargets.Members, AudioPatchTargets.Invariants);

        Assert.True(report.AllPassed, string.Join(Environment.NewLine, report.Failures.Select(f => $"{f.Id}: {f.Detail}")));
    }

    [Fact]
    public void The_installed_PlaySoundAtInternal_has_the_cap_and_the_range_check_the_transpiler_changes()
    {
        GameAssemblies? game = GameInstall.Value;
        if (game is null)
        {
            Assert.Skip("VINTAGE_STORY is not set to a game install");
        }

        Type clientMain = game.Lib.GetType("Vintagestory.Client.NoObf.ClientMain", throwOnError: true)!;
        MethodInfo method = HarmonyLib.AccessTools.Method(clientMain, "PlaySoundAtInternal");
        Assert.Equal("range", method.GetParameters()[VintageStorySpatialAudio.Takeover.PlatformPatches.RangeArgument - 1].Name);
        List<HarmonyLib.CodeInstruction> original = HarmonyLib.PatchProcessor.GetOriginalInstructions(method);
        HarmonyLib.CodeInstruction[] patched = [.. VintageStorySpatialAudio.Takeover.PlatformPatches.RemoveSoundCap(original)];
        Assert.True(VintageStorySpatialAudio.Takeover.PlatformPatches.SoundCapRemoved);
        Assert.True(VintageStorySpatialAudio.Takeover.PlatformPatches.RangeWidened);
        Assert.Equal(original.Count + 2, patched.Length);
    }

    [Fact]
    public void The_installed_OnClientFrame_still_skips_the_creatures_that_are_not_drawn()
    {
        GameAssemblies? game = GameInstall.Value;
        if (game is null)
        {
            Assert.Skip("VINTAGE_STORY is not set to a game install");
        }

        // The reason for the patch: vanilla advances a creature's animations, and so triggers the
        // footsteps its frames carry, only while something is drawing it. If a game update fixes
        // that, this test fails and SoundsFromUnseenCreatures should go with it.
        MethodInfo onClientFrame = HarmonyLib.AccessTools.Method(typeof(Vintagestory.API.Common.AnimationManager), "OnClientFrame");
        Assert.Equal("dt", onClientFrame.GetParameters()[0].Name);  // the prefix binds by name
        FieldInfo[] read = [.. HarmonyLib.PatchProcessor.GetOriginalInstructions(onClientFrame)
            .Select(i => i.operand as FieldInfo)
            .Where(f => f is not null && f.DeclaringType == typeof(Vintagestory.API.Common.Entities.Entity))!];
        Assert.Contains(read, f => f.Name == "IsRendered");
        Assert.Contains(read, f => f.Name == "IsShadowRendered");
    }

    [Fact]
    public void Native_engine_loads_and_passes_its_self_test()
    {
        NativeTestEnvironment.RequireNatives();

        EngineVersion version = AudioEngine.GetVersion();
        Assert.Equal(VsaNative.AbiVersion, version.AbiVersion);
        Assert.Equal(new Version(4, 8, 1), version.SteamAudio);

        var log = new ListLog();
        using AudioEngine engine = AudioEngine.Create(new EngineOptions(), log);
        Assert.NotEqual(RayTracer.Auto, engine.Info.ActiveRayTracer);

        SelfTestResult result = engine.RunSelfTest();
        Assert.True(result.Passed, $"occlusion through wall {result.OcclusionThroughWall}, clear {result.OcclusionClearPath}");
        Assert.Contains(log.Lines, l => l.Contains("context created", StringComparison.Ordinal));

        // Only one engine per process.
        NativeException second = Assert.Throws<NativeException>(() => AudioEngine.Create(new EngineOptions(), null));
        Assert.Equal("AlreadyExists", second.Result);
    }


    private sealed class ListLog : IEngineLog
    {
        private readonly Lock gate = new();
        private readonly List<string> lines = [];

        public IReadOnlyList<string> Lines
        {
            get
            {
                lock (gate)
                {
                    return [.. lines];
                }
            }
        }

        public void Write(EngineLogLevel level, string message)
        {
            lock (gate)
            {
                lines.Add(message);
            }
        }
    }
}

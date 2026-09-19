using System.Reflection;
using System.Runtime.Loader;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio.Tests;

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

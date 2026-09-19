using Vintagestory.API.Client;
using Vintagestory.API.Common;
using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Diagnostics;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio;

/// <summary>
/// Entry point. Phase 0: loads the native engine, runs its self-test and verifies every
/// game integration point, then reports. Nothing in the game is patched yet.
/// </summary>
public sealed class SteamAudioModSystem : ModSystem
{
    private AudioEngine? engine;
    private StatusReport status = new();

    public override bool ShouldLoad(EnumAppSide forSide) => forSide == EnumAppSide.Client;

    // Runs before other client mod systems start, i.e. before any world sound exists.
    // Phase 2 applies the audio takeover here.
    public override double ExecuteOrder() => 0.0;

    public override void StartClientSide(ICoreClientAPI api)
    {
        SteamAudioConfig config = LoadConfig(api);
        status = BringUp(config, Mod.Logger);

        foreach (string line in status.Render().Split('\n'))
        {
            Mod.Logger.Notification(line.TrimEnd('\r'));
        }

        api.ChatCommands.Create("steamaudio")
            .WithDescription("Steam Audio engine status")
            .BeginSubCommand("status")
                .WithDescription("Show engine, self-test and game integration status")
                .HandleWith(_ => TextCommandResult.Success(status.Render()))
            .EndSubCommand()
            .BeginSubCommand("targets")
                .WithDescription("List every verified game integration point")
                .HandleWith(_ => TextCommandResult.Success(status.Render(includePassingTargets: true)))
            .EndSubCommand();
    }

    public override void Dispose()
    {
        // World exit: the next session creates a fresh engine (only one may exist per process).
        engine?.Dispose();
        engine = null;
        base.Dispose();
    }

    private StatusReport BringUp(SteamAudioConfig config, ILogger logger)
    {
        VerificationReport verification = PatchTargetVerifier.Verify(
            GameAssemblies.FromCurrentProcess(), AudioPatchTargets.Members, AudioPatchTargets.Invariants);

        try
        {
            NativeLibraryResolver.Register(NativeLibraryResolver.DefaultNativeDirectory(typeof(SteamAudioModSystem).Assembly.Location));
            EngineVersion version = AudioEngine.GetVersion();
            engine = AudioEngine.Create(config.ToEngineOptions(), new GameLoggerEngineLog(logger));
            SelfTestResult? selfTest = config.RunSelfTestOnStartup ? engine.RunSelfTest() : null;
            return new StatusReport { Version = version, Engine = engine.Info, SelfTest = selfTest, Verification = verification };
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException
                                       or NativeException or InvalidOperationException or PlatformNotSupportedException)
        {
            logger.Error("Steam Audio engine failed to start; vanilla audio remains active. {0}", ex);
            engine?.Dispose();
            engine = null;
            return new StatusReport { EngineError = ex.Message, Verification = verification };
        }
    }

    private SteamAudioConfig LoadConfig(ICoreAPI api)
    {
        try
        {
            SteamAudioConfig? config = api.LoadModConfig<SteamAudioConfig>(SteamAudioConfig.FileName);
            if (config is null)
            {
                config = new SteamAudioConfig();
                api.StoreModConfig(config, SteamAudioConfig.FileName);
            }

            return config;
        }
        catch (Exception ex)
        {
            Mod.Logger.Error("Could not read {0}; using defaults. {1}", SteamAudioConfig.FileName, ex.Message);
            return new SteamAudioConfig();
        }
    }
}

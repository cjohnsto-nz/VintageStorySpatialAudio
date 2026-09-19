using System.Globalization;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Diagnostics;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio;

/// <summary>
/// Entry point. Loads the native engine, runs its self-test and verifies every game integration
/// point, then reports. Nothing in the game is patched yet (the takeover is Phase 2); the Phase 1
/// test commands play game sounds through our own engine and device alongside vanilla audio.
/// </summary>
public sealed class SteamAudioModSystem : ModSystem, IDisposable
{
    private AudioEngine? engine;
    private TestPlayback? playback;
    private long tickListener = -1;
    private ICoreClientAPI? capi;
    private StatusReport status = new();

    public override bool ShouldLoad(EnumAppSide forSide) => forSide == EnumAppSide.Client;

    // Runs before other client mod systems start, i.e. before any world sound exists.
    // Phase 2 applies the audio takeover here.
    public override double ExecuteOrder() => 0.0;

    public override void StartClientSide(ICoreClientAPI api)
    {
        capi = api;
        SteamAudioConfig config = LoadConfig(api);
        status = BringUp(config, Mod.Logger);
        if (engine is not null)
        {
            playback = new TestPlayback(engine, Mod.Logger, config.TestOutputDevice);
            tickListener = api.Event.RegisterGameTickListener(_ => TickPlayback(), 100);
        }

        foreach (string line in status.Render().Split('\n'))
        {
            Mod.Logger.Notification(line.TrimEnd('\r'));
        }

        CommandArgumentParsers parsers = api.ChatCommands.Parsers;
        api.ChatCommands.Create("steamaudio")
            .WithDescription("Steam Audio engine status and test playback")
            .BeginSubCommand("status")
                .WithDescription("Show engine, self-test and game integration status")
                .HandleWith(_ => TextCommandResult.Success(status.Render()))
            .EndSubCommand()
            .BeginSubCommand("targets")
                .WithDescription("List every verified game integration point")
                .HandleWith(_ => TextCommandResult.Success(status.Render(includePassingTargets: true)))
            .EndSubCommand()
            .BeginSubCommand("stats")
                .WithDescription("Engine telemetry: output, voices, render time, limiter")
                .HandleWith(_ => WithEngine(() => RenderStats(engine!.GetStats())))
            .EndSubCommand()
            .BeginSubCommand("devices")
                .WithDescription("List playback devices")
                .HandleWith(_ => WithEngine(() => playback!.ListDevices()))
            .EndSubCommand()
            .BeginSubCommand("play")
                .WithDescription("Play a game sound through the Steam Audio engine, e.g. .steamaudio play effect/woodswitch 1 1")
                .WithArgs(parsers.Word("sound"), parsers.OptionalFloat("volume", 1f), parsers.OptionalFloat("pitch", 1f))
                .HandleWith(args => WithEngine(() => playback!.Play(api, (string)args[0], (float)args[1], (float)args[2])))
            .EndSubCommand()
            .BeginSubCommand("stop")
                .WithDescription("Stop every test sound")
                .HandleWith(_ => WithEngine(() => $"Stopped {playback!.StopAll()} sound(s)."))
            .EndSubCommand();
    }

    public override void Dispose()
    {
        if (tickListener >= 0)
        {
            capi?.Event.UnregisterGameTickListener(tickListener);
            tickListener = -1;
        }

        playback?.Dispose();
        playback = null;
        // World exit: the next session creates a fresh engine (only one may exist per process).
        engine?.Dispose();
        engine = null;
        base.Dispose();
    }

    private TextCommandResult WithEngine(Func<string> action)
    {
        if (engine is null || playback is null)
        {
            return TextCommandResult.Error("The Steam Audio engine is not running; see .steamaudio status.");
        }

        try
        {
            return TextCommandResult.Success(action());
        }
        catch (NativeException ex)
        {
            Mod.Logger.Error("Steam Audio command failed: {0}", ex.Message);
            return TextCommandResult.Error(ex.Message);
        }
    }

    private void TickPlayback()
    {
        try
        {
            playback?.Tick();
        }
        catch (NativeException ex)
        {
            Mod.Logger.Error("Steam Audio event polling failed: {0}", ex.Message);
        }
    }

    private static string RenderStats(EngineStats s) => string.Create(
        CultureInfo.InvariantCulture,
        $"Output: {(s.Output == OutputKind.Device ? $"'{s.DeviceName}'" : "none (offline)")}, {s.SampleRate} Hz, {s.Channels} ch, " +
        $"block {s.BlockFrames} frames ({s.BlockPeriodUs / 1000:0.00} ms), device period {s.DevicePeriodFrames}\n" +
        $"Voices: {s.ActiveVoices} active, {s.AllocatedVoices}/{s.MaxVoices} allocated\n" +
        $"Render: avg {s.RenderTimeAvgUs:0} us, max {s.RenderTimeMaxUs:0} us per block ({s.RenderTimeMaxUs / Math.Max(1, s.BlockPeriodUs):P0} of the period); " +
        $"{s.BlocksRendered} blocks, {s.Overloads} overloads, {s.StreamUnderruns} stream underruns\n" +
        $"Limiter: deepest reduction {s.LimiterPeakReductionDb:0.0} dB since the last read");

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

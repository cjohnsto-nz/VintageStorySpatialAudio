using System.Globalization;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Debugging;
using VintageStorySteamAudio.Diagnostics;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;
using VintageStorySteamAudio.Takeover;
using VintageStorySteamAudio.World;

namespace VintageStorySteamAudio;

/// <summary>
/// Entry point. In the earliest client phase: verify every game integration point, start the
/// native engine, run its self-test and, if all of that succeeded, take over the game's audio for
/// this world session (ADR 0006). On world exit the audio is handed back to vanilla.
/// </summary>
public sealed class SteamAudioModSystem : ModSystem, IDisposable
{
    /// <summary>Mods that replace or drive the game's audio themselves (docs/PLAN.md §6.5).</summary>
    private static readonly string[] IncompatibleMods = ["vintagestorysurroundsound", "vintagestoryacousticlab"];

    private AudioEngine? engine;
    private AudioTakeover? takeover;
    private TestPlayback? playback;
    private PerfReporter? perf;
    private SteamAudioConfig? config;
    private bool muteDirect;
    private bool mutePaths;
    private bool muteReflections;
    private FrameProbe? frameProbe;
    private WorldAcoustics? world;
    private SceneDebugTools? sceneTools;
    private SpeakerTest? speakerTest;
    private long tickListener = -1;
    private ICoreClientAPI? capi;
    private StatusReport status = new();

    public override bool ShouldLoad(EnumAppSide forSide) => forSide == EnumAppSide.Client;

    // First among client mod systems.
    public override double ExecuteOrder() => 0.0;

    /// <summary>The earliest client phase: before any world sound exists.</summary>
    public override void StartPre(ICoreAPI api)
    {
        if (api is not ICoreClientAPI clientApi)
        {
            return;
        }

        capi = clientApi;
        SteamAudioConfig config = LoadConfig(clientApi);
        status = BringUp(config, Mod.Logger, api.GetOrCreateDataPath("ModConfig"));
        if (engine is not null && status.TakeoverPossible)
        {
            status = TakeOver(config, clientApi, status);
        }

        foreach (string line in status.Render().Split('\n'))
        {
            Mod.Logger.Notification(line.TrimEnd('\r'));
        }
    }

    public override void StartClientSide(ICoreClientAPI api)
    {
        // Mods that load after us may patch the same calls; they would fight our patches all session.
        if (takeover?.ForeignPatches() is string conflict)
        {
            Mod.Logger.Warning("{0}. Handing audio back to vanilla for this session.", conflict);
            takeover.Dispose();
            takeover = null;
            status = status with { TakeoverActive = false, TakeoverNote = conflict };
        }

        if (engine is not null)
        {
            SteamAudioConfig config = LoadConfig(api);
            this.config = config;
            playback = new TestPlayback(engine, Mod.Logger, config.TestOutputDevice);
            PerfMonitor.Instance.AttachThread();
            perf = new PerfReporter(PerfMonitor.Instance, engine, () => takeover?.Session);
            // Frames are counted whether or not we play the audio, so a TakeOverGameAudio-off run
            // is a baseline measured the same way (docs/investigations/performance.md).
            frameProbe = new FrameProbe();
            api.Event.RegisterRenderer(frameProbe, EnumRenderStage.Before, "vssteamaudio-frames");
            tickListener = api.Event.RegisterGameTickListener(_ => TickPlayback(), 100);
            if (config.BuildWorldScene)
            {
                StartWorldScene(api, config);
            }
        }

        CommandArgumentParsers parsers = api.ChatCommands.Parsers;
        api.ChatCommands.Create("steamaudio")
            .WithDescription("Steam Audio engine status and test playback")
            .BeginSubCommand("status")
                .WithDescription("Show engine, self-test, game integration and takeover status")
                .HandleWith(_ => TextCommandResult.Success(status.Render()))
            .EndSubCommand()
            .BeginSubCommand("targets")
                .WithDescription("List every verified game integration point")
                .HandleWith(_ => TextCommandResult.Success(status.Render(includePassingTargets: true)))
            .EndSubCommand()
            .BeginSubCommand("stats")
                .WithDescription("Engine telemetry: output, voices, render time, limiter")
                .HandleWith(_ => WithEngine(RenderStats))
            .EndSubCommand()
            .BeginSubCommand("devices")
                .WithDescription("List playback devices")
                .HandleWith(_ => WithEngine(() => playback!.ListDevices()))
            .EndSubCommand()
            .BeginSubCommand("play")
                .WithDescription("Play a game sound straight through the engine, e.g. .steamaudio play effect/woodswitch 1 1")
                .WithArgs(parsers.Word("sound"), parsers.OptionalFloat("volume", 1f), parsers.OptionalFloat("pitch", 1f))
                .HandleWith(args => WithEngine(() => playback!.Play(api, (string)args[0], (float)args[1], (float)args[2])))
            .EndSubCommand()
            .BeginSubCommand("stop")
                .WithDescription("Stop every sound started with .steamaudio play")
                .HandleWith(_ => WithEngine(() => $"Stopped {playback!.StopAll()} sound(s)."))
            .EndSubCommand()
            .BeginSubCommand("speakertest")
                .WithDescription("Noise from each 7.1.4 speaker position in turn, then overhead; '.steamaudio speakertest stop' ends it")
                .WithArgs(parsers.OptionalWord("stop"))
                .HandleWith(args => WithEngine(() => SpeakerTestCommand(api, args[0] as string)))
            .EndSubCommand()
            .BeginSubCommand("solo")
                .WithDescription("Debugging: hear only the sounds whose name contains this (e.g. '.steamaudio solo anvil'), listed and drawn with the legs of their way round; '.steamaudio solo off' ends it")
                .WithArgs(parsers.OptionalWord("name"))
                .HandleWith(args => WithEngine(() => SoloCommand(args[0] as string)))
            .EndSubCommand()
            .BeginSubCommand("mute")
                .WithDescription("Debugging: silence one way a sound reaches you -- direct, paths or reflections (each toggles) -- to hear which one a leak comes by; '.steamaudio mute off' restores all")
                .WithArgs(parsers.OptionalWord("way"))
                .HandleWith(args => WithEngine(() => MuteCommand(args[0] as string)))
            .EndSubCommand()
            .BeginSubCommand("config")
                .WithDescription("The settings file: where it is, and 'reset' to put every setting back to its default (the new defaults of a new version included)")
                .WithArgs(parsers.OptionalWord("action"))
                .HandleWith(args => WithEngine(() => ConfigCommand(api, args[0] as string)))
            .EndSubCommand()
            .BeginSubCommand("perf")
                .WithDescription("What the mod costs since the last reset: our main-thread time per frame by section, every thread's share of a core, memory; 'reset' starts a new window")
                .WithArgs(parsers.OptionalWord("action"))
                .HandleWith(args => WithEngine(() => PerfCommand(args[0] as string)))
            .EndSubCommand()
            .BeginSubCommand("scene")
                .WithDescription("The acoustic scene: status, sounds [page|off] (every sound and how it reaches you), or wire|faces|bounds|sources|rays|paths|off (overlay, Ctrl+F7 cycles), radius N, legend, export (OBJ), reload (materials)")
                .WithArgs(parsers.OptionalWord("action"), parsers.OptionalWord("value"))
                .HandleWith(args => WithEngine(() => sceneTools is null
                    ? "The world scene is off (BuildWorldScene in " + SteamAudioConfig.FileName + ")."
                    : sceneTools.Command(args[0] as string, args[1] as string)))
            .EndSubCommand()
            .BeginSubCommand("reverb")
                .WithDescription("Reverb from the world: status, gain N (all of it, 0-4, 1 = as simulated), early N (early reflections), tail N (the reverb after them), rays (show sound paths)")
                .WithArgs(parsers.OptionalWord("action"), parsers.OptionalWord("value"))
                .HandleWith(args => WithEngine(() => sceneTools is null
                    ? "The world scene is off (BuildWorldScene in " + SteamAudioConfig.FileName + "): nothing to reflect off."
                    : sceneTools.ReverbCommand(args[0] as string, args[1] as string)))
            .EndSubCommand();
    }

    public override void Dispose()
    {
        if (tickListener >= 0)
        {
            capi?.Event.UnregisterGameTickListener(tickListener);
            tickListener = -1;
        }

        if (frameProbe is not null)
        {
            capi?.Event.UnregisterRenderer(frameProbe, EnumRenderStage.Before);
            frameProbe = null;
        }

        speakerTest?.Dispose();
        speakerTest = null;
        sceneTools?.Dispose();
        sceneTools = null;
        world?.Dispose();
        world = null;
        playback?.Dispose();
        playback = null;
        // World exit: hand audio back to vanilla before the engine goes (only one may exist per process).
        takeover?.Dispose();
        takeover = null;
        engine?.Dispose();
        engine = null;
        base.Dispose();
    }

    private void StartWorldScene(ICoreClientAPI api, SteamAudioConfig config)
    {
        AudioEngine audio = engine!;
        world = new WorldAcoustics(
            api,
            audio,
            Mod.Logger,
            (x, y, z) =>
            {
                // With the takeover, the session re-sends the listener and sounds relative to it.
                if (takeover is not null)
                {
                    takeover.Session.SetOrigin(x, y, z);
                }
                else
                {
                    audio.SetSceneOrigin(x, y, z);
                }
            },
            config.SceneFullRadiusChunks,
            config.SceneLodRadiusChunks,
            config.SceneVerticalRadiusChunks,
            config.SceneBudgetMs);
        sceneTools = new SceneDebugTools(api, audio, world, voice => takeover?.Session.DescribeVoice(voice))
        {
            DebugState = DebugStateText,
        };
        api.Event.LevelFinalize += () =>
        {
            try
            {
                world?.Start();
            }
            catch (NativeException ex)
            {
                Mod.Logger.Error("Steam Audio: the world scene could not start: {0}", ex.Message);
            }
        };
    }

    private StatusReport TakeOver(SteamAudioConfig config, ICoreClientAPI api, StatusReport report)
    {
        if (!config.TakeOverGameAudio)
        {
            return report with { TakeoverNote = $"TakeOverGameAudio is off in {SteamAudioConfig.FileName}" };
        }

        string[] conflicting = IncompatibleMods.Where(api.ModLoader.IsModEnabled).ToArray();
        if (conflicting.Length > 0)
        {
            return report with { TakeoverNote = $"incompatible audio mod enabled: {string.Join(", ", conflicting)}" };
        }

        try
        {
            takeover = AudioTakeover.Begin(GameAssemblies.FromCurrentProcess(), engine!, config, api, Mod.Logger);
            return report with { TakeoverActive = true };
        }
        catch (Exception ex) when (ex is NativeException or InvalidOperationException or MemberAccessException
                                       or System.Reflection.TargetInvocationException or HarmonyLib.HarmonyException
                                       or NotSupportedException or ArgumentException)
        {
            Mod.Logger.Error("Steam Audio takeover failed; vanilla audio stays in charge. {0}", ex);
            takeover = null;
            return report with { TakeoverNote = "failed: " + ex.Message };
        }
    }

    private string SpeakerTestCommand(ICoreClientAPI api, string? argument)
    {
        if (string.Equals(argument, "stop", StringComparison.OrdinalIgnoreCase))
        {
            bool running = speakerTest?.Running == true;
            speakerTest?.Stop();
            return running ? "Speaker test stopped." : "No speaker test is running.";
        }

        if (playback!.EnsureDevice() is string error)
        {
            return error;
        }

        speakerTest ??= new SpeakerTest(engine!);
        return speakerTest.Start(api);
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
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.Playback);
        try
        {
            // Without the takeover nothing else drains the engine's events.
            if (takeover is null)
            {
                playback?.Tick();
            }
        }
        catch (NativeException ex)
        {
            Mod.Logger.Error("Steam Audio event polling failed: {0}", ex.Message);
        }
    }

    /// <summary>"'name'", "'name' via Windows Spatial Audio (7.1.4)" or "none (offline)".</summary>
    internal static string DescribeOutput(EngineStats s) => s.Output switch
    {
        OutputKind.Device => $"'{s.DeviceName}'",
        OutputKind.Spatial => $"'{s.DeviceName}' via Windows Spatial Audio (7.1.4)",
        _ => "none (offline)",
    };

    /// <summary>".steamaudio solo [name|off]".</summary>
    private string SoloCommand(string? name)
    {
        if (takeover is null)
        {
            return "The mod is not playing the game's audio.";
        }

        AudioSession session = takeover.Session;
        if (name is null)
        {
            return session.Solo is null ? "Nothing is soloed. Usage: .steamaudio solo <part of a sound's name>|off" : $"Soloed: '{session.Solo}'.";
        }

        if (string.Equals(name, "off", StringComparison.OrdinalIgnoreCase))
        {
            session.Solo = null;
            return "Solo off: every sound is heard again.";
        }

        session.Solo = name;
        sceneTools?.ShowSolo();
        return $"Only sounds with '{name}' in their name are heard. The panel lists them, and they alone are drawn: "
            + "the line to the sound, the arrow to where it comes in, and the legs of its way round. "
            + "'.steamaudio mute direct|paths|reflections' silences one way at a time; '.steamaudio solo off' ends this.";
    }

    /// <summary>".steamaudio mute [direct|paths|reflections|off]".</summary>
    private string MuteCommand(string? way)
    {
        switch ((way ?? string.Empty).ToLowerInvariant())
        {
            case "direct":
                muteDirect = !muteDirect;
                break;
            case "paths":
            case "path":
                mutePaths = !mutePaths;
                break;
            case "reflections":
            case "reverb":
                muteReflections = !muteReflections;
                break;
            case "off":
                muteDirect = mutePaths = muteReflections = false;
                break;
            case "":
                return "Muted: " + MutedText() + ". Usage: .steamaudio mute direct|paths|reflections|off";
            default:
                return "Usage: .steamaudio mute direct|paths|reflections|off";
        }

        engine!.SetRouteGains(muteDirect ? 0f : 1f, mutePaths ? 0f : 1f);
        engine.SetReflectionGain(muteReflections ? 0f : (config?.ReflectionGainClamped() ?? 0.1f));
        return "Muted: " + MutedText() + ".";
    }

    private string MutedText()
    {
        var muted = new List<string>();
        if (muteDirect)
        {
            muted.Add("the direct sound");
        }

        if (mutePaths)
        {
            muted.Add("the ways round");
        }

        if (muteReflections)
        {
            muted.Add("the reflections");
        }

        return muted.Count == 0 ? "nothing" : string.Join(", ", muted);
    }

    /// <summary>What the debugging commands have changed, for the inspector's heading.</summary>
    private string? DebugStateText()
    {
        string? solo = takeover?.Session.Solo;
        if (solo is null && !muteDirect && !mutePaths && !muteReflections)
        {
            return null;
        }

        return (solo is null ? "every sound" : $"only '{solo}'") + "; muted: " + MutedText();
    }

    /// <summary>".steamaudio config [reset]".</summary>
    private string ConfigCommand(ICoreClientAPI api, string? action)
    {
        if (config is null)
        {
            return "The engine is not running.";
        }

        string path = Path.Combine(api.GetOrCreateDataPath("ModConfig"), SteamAudioConfig.FileName);
        if (action is null)
        {
            return $"Settings: {path}\n" + string.Create(
                CultureInfo.InvariantCulture,
                $"Reflections: {config.ReflectionQuality}, {config.ReflectionSources} places, {config.ReflectionRays} rays x {config.ReflectionBounces} bounces, " +
                $"{config.ReflectionDurationSeconds:0.##} s order {config.ReflectionOrder}, {config.ReflectionRateHz} Hz, {config.ReflectionThreads} threads, gain {config.ReflectionGain:0.##}\n" +
                $"Pathing: {config.PathingRangeBlocks} x {config.PathingHeightBlocks} x {config.PathingRangeBlocks} blocks, probes {config.PathingProbeSpacing:0.#} m apart up to {config.PathingMaxProbes}, " +
                $"{config.PathingRateHz} Hz, {config.PathingSources} sounds\n" +
                $"Occlusion: {config.OcclusionSamples} samples at {config.OcclusionRateHz} Hz\n") +
                "Every value is written out in full; edit the file and restart. '.steamaudio config reset' restores the defaults.";
        }

        if (!string.Equals(action, "reset", StringComparison.OrdinalIgnoreCase))
        {
            return "Usage: .steamaudio config [reset]";
        }

        config.ResetToDefaults(EngineDefaults.Read());
        api.StoreModConfig(config, SteamAudioConfig.FileName);
        Mod.Logger.Notification("{0} was reset to the defaults.", SteamAudioConfig.FileName);
        return $"Settings reset to the defaults and written to {path}. Restart the game to apply them.";
    }

    /// <summary>
    /// ".steamaudio perf [reset]". The report also goes to client-main.log: chat text cannot be
    /// copied out of the game, and these numbers are read afterwards, not in the moment.
    /// </summary>
    private string PerfCommand(string? action)
    {
        if (perf is null)
        {
            return "The engine is not running.";
        }

        string text = perf.Command(action);
        if (action is not null)
        {
            return text;
        }

        foreach (string line in text.Split('\n'))
        {
            Mod.Logger.Notification("[perf] {0}", line);
        }

        return text + "\n(also written to client-main.log)";
    }

    private string RenderStats()
    {
        EngineStats s = engine!.GetStats();
        string text = string.Create(
            CultureInfo.InvariantCulture,
            $"Output: {DescribeOutput(s)}, {s.SampleRate} Hz, {s.Channels} ch, " +
            $"block {s.BlockFrames} frames ({s.BlockPeriodUs / 1000:0.00} ms), device period {s.DevicePeriodFrames}\n" +
            $"Voices: {s.ActiveVoices} active ({s.RealVoices} positional with effects, {s.VirtualVoices} virtual), " +
            $"{s.AllocatedVoices}/{s.MaxVoices} allocated\n" +
            $"Render: avg {s.RenderTimeAvgUs:0} us, max {s.RenderTimeMaxUs:0} us per block ({s.RenderTimeMaxUs / Math.Max(1, s.BlockPeriodUs):P0} of the period); " +
            $"{s.BlocksRendered} blocks, {s.Overloads} overloads, {s.StreamUnderruns} stream underruns\n" +
            $"Limiter: deepest reduction {s.LimiterPeakReductionDb:0.0} dB since the last read");
        if (takeover is not null)
        {
            AudioSession session = takeover.Session;
            text += string.Create(
                CultureInfo.InvariantCulture,
                $"\nGame sounds: {session.SoundCount} ({session.PendingCount} waiting for data), " +
                $"{session.Assets.Count} assets ({session.Assets.MemoryBytes / (1024.0 * 1024.0):0.0} MB), " +
                $"250-sound cap {(takeover.SoundCapRemoved ? "removed" : "STILL ACTIVE")}");
            text += takeover.EntitySounds is { } entitySounds
                ? string.Create(
                    CultureInfo.InvariantCulture,
                    $"\nFollowing entities: {entitySounds.Count} sounds ({entitySounds.InferredCount} matched by position), {entitySounds.ExpectedCount} awaited")
                : "\nFollowing entities: off";
        }

        return text;
    }

    private StatusReport BringUp(SteamAudioConfig config, ILogger logger, string modConfigDirectory)
    {
        VerificationReport verification = PatchTargetVerifier.Verify(
            GameAssemblies.FromCurrentProcess(), AudioPatchTargets.Members, AudioPatchTargets.Invariants);

        try
        {
            NativeLibraryResolver.Register(NativeLibraryResolver.DefaultNativeDirectory(typeof(SteamAudioModSystem).Assembly.Location));
            EngineVersion version = AudioEngine.GetVersion();
            engine = AudioEngine.Create(config.ToEngineOptions(modConfigDirectory), new GameLoggerEngineLog(logger));
            engine.SetReflectionGain(config.ReflectionGainClamped());
            (float early, float tail) = config.ReflectionMixClamped();
            engine.SetReflectionMix(early, tail);
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
            bool fresh = config is null;
            config ??= new SteamAudioConfig();
            if (config.Migrate() && !fresh)
            {
                Mod.Logger.Notification("{0} was written by an earlier version; defaults that changed since were updated.", SteamAudioConfig.FileName);
            }

            // The file holds what the engine will actually do, not zeros standing for defaults
            // (ADR 0018), and gains the settings a new version added. The defaults come from the
            // native library, and this is the first thing in a session to call into it: without
            // the resolver it is not found, and the whole settings file was thrown away for defaults.
            NativeLibraryResolver.Register(NativeLibraryResolver.DefaultNativeDirectory(typeof(SteamAudioModSystem).Assembly.Location));
            if (config.Populate(EngineDefaults.Read()) && !fresh)
            {
                Mod.Logger.Notification("{0}: settings without a value were written out in full.", SteamAudioConfig.FileName);
            }

            api.StoreModConfig(config, SteamAudioConfig.FileName);
            return config;
        }
        catch (Exception ex)
        {
            Mod.Logger.Error("Could not read {0}; using defaults. {1}", SteamAudioConfig.FileName, ex.Message);
            return new SteamAudioConfig();
        }
    }
}

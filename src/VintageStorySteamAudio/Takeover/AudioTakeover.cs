using System.Collections;
using System.Reflection;
using HarmonyLib;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using Vintagestory.Client.NoObf;
using VintageStorySteamAudio.Config;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio.Takeover;

/// <summary>
/// Owns the game's audio for one world session (ADR 0006).
/// <para>
/// <b>Takeover</b>: open our device, patch the platform seam, move the intro music (if still
/// playing) onto the engine at its position, dispose vanilla's sources and close OpenAL.
/// </para>
/// <para>
/// <b>Hand-back</b> (<see cref="Dispose"/>): release every voice, unpatch, make vanilla decode the
/// samples we emptied again, and reopen OpenAL through the original StartAudio().
/// </para>
/// Every member used here is in <see cref="AudioPatchTargets"/> and verified before this class is
/// touched; any failure during takeover is rolled back and leaves vanilla audio in charge.
/// </summary>
internal sealed class AudioTakeover : IDisposable
{
    private const string HarmonyId = "vssteamaudio.takeover";
    private const long SettingsPollMs = 250;

    private readonly ICoreClientAPI api;
    private readonly ILogger logger;
    private readonly AudioSession session;
    private readonly Harmony harmony = new(HarmonyId);
    private readonly Members members;
    private readonly HashSet<string> undecodable = new(StringComparer.Ordinal);
    private IList<string> deviceNames = [];
    private long nextSettingsPoll;
    private bool disposed;

    private AudioTakeover(ICoreClientAPI api, ILogger logger, AudioSession session, Members members)
    {
        this.api = api;
        this.logger = logger;
        this.session = session;
        this.members = members;
    }

    public AudioSession Session => session;

    /// <summary>Whether vanilla's 250-sound cap was found and removed.</summary>
    public bool SoundCapRemoved { get; private set; }

    public string CurrentDevice { get; private set; } = string.Empty;

    /// <summary>The platform's master level (0..1), as the game sets it from the master slider.</summary>
    public float MasterLevel
    {
        get => masterLevel;
        set
        {
            masterLevel = value;
            PollSettings(force: true);
        }
    }

    private float masterLevel = 1f;

    /// <summary>
    /// Takes over the game's audio. Throws (after rolling back) if any step fails; the caller keeps
    /// vanilla audio then.
    /// </summary>
    public static AudioTakeover Begin(GameAssemblies game, AudioEngine engine, SteamAudioConfig config, ICoreClientAPI api, ILogger logger)
    {
        ArgumentNullException.ThrowIfNull(config);
        Members members = Members.Resolve(game);
        var session = new AudioSession(engine, logger, config.CategoryTrimsDb())
        {
            ListenerBackwardOffset = float.IsFinite(config.ListenerBackwardOffset) ? Math.Clamp(config.ListenerBackwardOffset, 0f, 3f) : 0f,
        };
        var takeover = new AudioTakeover(api, logger, session, members);
        try
        {
            string? conflict = takeover.ForeignPatches();
            if (conflict is not null)
            {
                throw new InvalidOperationException(conflict);
            }

            takeover.OpenDevice(api.Settings.String[SoundCategories.DeviceSetting]);
            takeover.masterLevel = api.Settings.Int[SoundCategories.MasterSetting] / 100f;
            takeover.PollSettings(force: true);
            takeover.Patch();
            PlatformPatches.Active = takeover;
            takeover.MigrateIntroMusic();
            takeover.CloseOpenAl();
            return takeover;
        }
        catch
        {
            takeover.Dispose();
            throw;
        }
    }

    /// <summary>Hands audio back to vanilla. Safe to call more than once.</summary>
    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        PlatformPatches.Active = null;
        session.Dispose();
        harmony.UnpatchAll(HarmonyId);
        ResetEmptiedSamples();
        ReopenOpenAl();
        logger.Notification("Audio handed back to vanilla OpenAL.");
    }

    // ---- called by the patches ----

    /// <summary>ClientPlatformWindows.CreateAudioData: decode natively, give the game PCM-less metadata.</summary>
    public AudioData DecodeForGame(IAsset asset)
    {
        string location = asset.Location.ToString();
        var meta = new AudioMetaData(asset) { Pcm = [], BitsPerSample = 16, Loaded = 2 };
        byte[]? data = asset.Data;
        if (data is null || !LooksLikeAudio(data))
        {
            // The game's sound table also holds non-audio files (sounds/soundconfig.json); vanilla
            // cannot decode those either. Remember them quietly.
            lock (undecodable)
            {
                undecodable.Add(location);
            }

            return meta;
        }

        try
        {
            AssetInfo info = session.Assets.GetOrDecode(location, data).Info;
            meta.Channels = info.Channels;
            meta.Rate = info.SampleRate;
        }
        catch (NativeException ex)
        {
            MarkUndecodable(location, ex.Message);
        }

        return meta;
    }

    /// <summary>ClientPlatformWindows.CreateAudio (both overloads).</summary>
    public ILoadedSound? CreateSound(SoundParams sound, AudioData data)
    {
        if (data is not AudioMetaData meta || meta.Asset is null)
        {
            return null;  // as vanilla
        }

        if (meta.Loaded == 0)
        {
            meta.Load();  // vanilla decodes synchronously here too ("game may stutter")
        }

        string location = meta.Asset.Location.ToString();
        return session.CreateSound(sound, () => Resolve(meta, location), Math.Max(1, meta.Channels));
    }

    /// <summary>ClientPlatformWindows.UpdateAudioListener: once per frame on the main thread.</summary>
    public void OnFrame(float posX, float posY, float posZ, float orientX, float orientY, float orientZ)
    {
        // Vanilla passes a flattened view (y = 0); use the player's full view direction when we can.
        Vec3f? view = api.World?.Player?.Entity?.Pos?.GetViewVector();
        if (view is null)
        {
            session.SetListener(posX, posY, posZ, orientX, orientY, orientZ);
        }
        else
        {
            session.SetListener(posX, posY, posZ, view.X, view.Y, view.Z);
        }

        session.Pump();
        PollSettings(force: false);
    }

    public IList<string> DeviceNames()
    {
        try
        {
            deviceNames = session.Engine.EnumerateDevices().Select(d => d.Name).ToList();
        }
        catch (NativeException ex)
        {
            logger.Warning("device enumeration failed: {0}", ex.Message);
        }

        return deviceNames;
    }

    public void SelectDevice(string? name)
    {
        try
        {
            OpenDevice(name);
        }
        catch (NativeException ex)
        {
            logger.Error("could not switch output to '{0}': {1}", name ?? "(default)", ex.Message);
        }
    }

    // ---- takeover steps ----

    private void OpenDevice(string? preferredName)
    {
        AudioDevice? device = null;
        if (!string.IsNullOrWhiteSpace(preferredName))
        {
            IReadOnlyList<AudioDevice> devices = session.Engine.EnumerateDevices();
            device = MatchDevice(devices, preferredName);
            if (device is null)
            {
                logger.Notification("no device matches '{0}'; using the system default", preferredName);
            }
        }

        session.Engine.OpenDevice(device);
        EngineStats stats = session.Engine.GetStats();
        CurrentDevice = stats.DeviceName;
        logger.Notification(
            "output: '{0}', {1} Hz, {2} channels, period {3} frames",
            stats.DeviceName, stats.SampleRate, stats.Channels, stats.DevicePeriodFrames);
    }

    /// <summary>
    /// Finds the device the game's setting names. The setting holds an OpenAL name
    /// ("OpenAL Soft on Speakers (…)"); ours come from the platform backend ("Speakers (…)").
    /// </summary>
    internal static AudioDevice? MatchDevice(IReadOnlyList<AudioDevice> devices, string name)
    {
        const string OpenAlPrefix = "OpenAL Soft on ";
        string wanted = name.StartsWith(OpenAlPrefix, StringComparison.Ordinal) ? name[OpenAlPrefix.Length..] : name;
        return devices.FirstOrDefault(d => string.Equals(d.Name, wanted, StringComparison.OrdinalIgnoreCase))
            ?? devices.FirstOrDefault(d => d.Name.Contains(wanted, StringComparison.OrdinalIgnoreCase)
                                           || wanted.Contains(d.Name, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Every game method the takeover patches.</summary>
    private IEnumerable<MethodBase> PatchedMethods() =>
    [
        members.StartAudio, members.CreateAudioData, members.CreateAudio, members.CreateAudioInGame, members.UpdateListener,
        members.Devices.GetMethod!, members.CurrentDevice.GetMethod!, members.CurrentDevice.SetMethod!,
        members.MasterLevel.GetMethod!, members.MasterLevel.SetMethod!, members.ChangeOutputDevice, members.PlaySoundAt,
    ];

    /// <summary>
    /// Other mods' Harmony patches on the methods we take over: a description of the conflict, or null.
    /// Two mods replacing the same audio calls cannot both work, so we step aside.
    /// </summary>
    public string? ForeignPatches()
    {
        var conflicts = new List<string>();
        foreach (MethodBase method in PatchedMethods())
        {
            Patches? info = Harmony.GetPatchInfo(method);
            if (info is null)
            {
                continue;
            }

            string[] owners = info.Prefixes.Concat(info.Postfixes).Concat(info.Transpilers).Concat(info.Finalizers)
                .Select(p => p.owner).Where(owner => owner != HarmonyId).Distinct().ToArray();
            if (owners.Length > 0)
            {
                conflicts.Add($"{method.DeclaringType?.Name}.{method.Name} by {string.Join(", ", owners)}");
            }
        }

        return conflicts.Count == 0 ? null : "other mods patch the game's audio: " + string.Join("; ", conflicts);
    }

    private void Patch()
    {
        HarmonyMethod Prefix(string name) => new(typeof(PlatformPatches).GetMethod(name, BindingFlags.Public | BindingFlags.Static));

        harmony.Patch(members.StartAudio, prefix: Prefix(nameof(PlatformPatches.StartAudio)));
        harmony.Patch(members.CreateAudioData, prefix: Prefix(nameof(PlatformPatches.CreateAudioData)));
        harmony.Patch(members.CreateAudio, prefix: Prefix(nameof(PlatformPatches.CreateAudio)));
        harmony.Patch(members.CreateAudioInGame, prefix: Prefix(nameof(PlatformPatches.CreateAudioInGame)));
        harmony.Patch(members.UpdateListener, prefix: Prefix(nameof(PlatformPatches.UpdateAudioListener)));
        harmony.Patch(members.Devices.GetMethod!, prefix: Prefix(nameof(PlatformPatches.GetAvailableAudioDevices)));
        harmony.Patch(members.CurrentDevice.GetMethod!, prefix: Prefix(nameof(PlatformPatches.GetCurrentAudioDevice)));
        harmony.Patch(members.CurrentDevice.SetMethod!, prefix: Prefix(nameof(PlatformPatches.SetCurrentAudioDevice)));
        harmony.Patch(members.MasterLevel.GetMethod!, prefix: Prefix(nameof(PlatformPatches.GetMasterSoundLevel)));
        harmony.Patch(members.MasterLevel.SetMethod!, prefix: Prefix(nameof(PlatformPatches.SetMasterSoundLevel)));
        harmony.Patch(members.ChangeOutputDevice, prefix: Prefix(nameof(PlatformPatches.ChangeOutputDevice)));
        harmony.Patch(members.PlaySoundAt, transpiler: new HarmonyMethod(typeof(PlatformPatches), nameof(PlatformPatches.RemoveSoundCap)));
        SoundCapRemoved = PlatformPatches.SoundCapRemoved;
        if (!SoundCapRemoved)
        {
            logger.Warning("the 250-sound cap was not found in PlaySoundAtInternal; it stays in place");
        }
    }

    /// <summary>The menu music may still be fading out as the world starts: carry it over at its position.</summary>
    private void MigrateIntroMusic()
    {
        if (members.IntroMusic.GetValue(null) is not ILoadedSound intro || intro is SteamAudioSound || intro.IsDisposed || intro.HasStopped)
        {
            return;
        }

        SoundParams soundParams = intro.Params;
        float position = intro.PlaybackPosition;
        bool paused = intro.IsPaused;
        if (soundParams?.Location is null || ScreenManagerAudio(soundParams.Location) is not AudioMetaData meta)
        {
            return;
        }

        ILoadedSound? twin = CreateSound(soundParams, meta);
        if (twin is null)
        {
            return;
        }

        twin.PlaybackPosition = position;
        twin.Start();
        if (paused)
        {
            twin.Pause();
        }

        members.IntroMusic.SetValue(null, twin);
        logger.Notification("carried the menu music over at {0:0.0} s", position);
    }

    private void CloseOpenAl()
    {
        members.DisposeAllSounds.Invoke(null, null);
        object? platform = members.PlatformInstance.GetValue(null);
        if (platform is not null && members.OpenAl.GetValue(platform) is { } openAl)
        {
            members.OpenAlDispose.Invoke(openAl, null);
            members.OpenAl.SetValue(platform, null);
        }
    }

    // ---- hand-back steps ----

    /// <summary>Samples we decoded natively have no PCM; make vanilla decode them again when next used.</summary>
    private void ResetEmptiedSamples()
    {
        try
        {
            if (members.SoundAudioData.GetValue(null) is not IDictionary table)
            {
                return;
            }

            object[] values;
            lock (table.SyncRoot)
            {
                values = [.. table.Values.Cast<object>()];
            }

            int reset = 0;
            foreach (object value in values)
            {
                if (value is AudioMetaData { Loaded: >= 2, Pcm.Length: 0 } meta)
                {
                    meta.Unload();
                    reset++;
                }
            }

            logger.VerboseDebug("{0} samples reset for vanilla", reset);
        }
        catch (Exception ex) when (ex is InvalidOperationException or TargetInvocationException)
        {
            logger.Warning("could not reset samples for vanilla: {0}", ex.Message);
        }
    }

    private void ReopenOpenAl()
    {
        try
        {
            object? platform = members.PlatformInstance.GetValue(null);
            if (platform is null || members.OpenAl.GetValue(platform) is not null)
            {
                return;
            }

            members.StartAudio.Invoke(platform, null);
            members.MasterLevel.SetValue(platform, api.Settings.Int[SoundCategories.MasterSetting] / 100f);
        }
        catch (TargetInvocationException ex)
        {
            logger.Error("reopening OpenAL failed: {0}", ex.InnerException?.Message ?? ex.Message);
        }
    }

    // ---- helpers ----

    private AudioAsset? Resolve(AudioMetaData meta, string location)
    {
        if (meta.Loaded < 2)
        {
            return null;  // still decoding on another thread
        }

        if (!session.Assets.TryGet(location, out AudioAsset asset))
        {
            if (meta.Pcm is { Length: > 0 } pcm)
            {
                // Decoded by vanilla before the takeover (e.g. the menu music).
                asset = session.Assets.GetOrWrapPcm(location, pcm, meta.Channels, meta.Rate, meta.BitsPerSample);
            }
            else if (IsUndecodable(location))
            {
                throw new NotSupportedException($"{location} could not be decoded");
            }
            else
            {
                // Emptied in an earlier session and not reset: decode again.
                meta.Unload();
                meta.Load();
                if (!session.Assets.TryGet(location, out asset))
                {
                    throw new NotSupportedException($"{location} could not be decoded");
                }
            }
        }

        meta.Loaded = 3;  // "in use": PlaySoundAt starts sounds only once their data reaches 3
        return asset;
    }

    private AudioData? ScreenManagerAudio(AssetLocation location) =>
        members.SoundAudioData.GetValue(null) is IDictionary table && table.Contains(location) ? table[location] as AudioData : null;

    private ILoadedSound? CreateSound(SoundParams soundParams, AudioMetaData meta) => CreateSound(soundParams, (AudioData)meta);

    private void MarkUndecodable(string location, string reason)
    {
        lock (undecodable)
        {
            if (!undecodable.Add(location))
            {
                return;
            }
        }

        logger.Warning("could not decode {0}: {1}", location, reason);
    }

    /// <summary>Ogg ("OggS") or RIFF WAVE, the only formats the game ships.</summary>
    internal static bool LooksLikeAudio(ReadOnlySpan<byte> data) =>
        data.StartsWith("OggS"u8) || (data.Length >= 12 && data.StartsWith("RIFF"u8) && data[8..12].SequenceEqual("WAVE"u8));

    private bool IsUndecodable(string location)
    {
        lock (undecodable)
        {
            return undecodable.Contains(location);
        }
    }

    private void PollSettings(bool force)
    {
        long now = Environment.TickCount64;
        if (!force && now < nextSettingsPoll)
        {
            return;
        }

        nextSettingsPoll = now + SettingsPollMs;
        var levels = SoundCategories.LevelSettings.ToDictionary(p => p.Key, p => api.Settings.Int[p.Value]);
        session.ApplyLevels(new AudioLevels((int)Math.Round(masterLevel * 100f), levels, api.Settings.Bool[SoundCategories.HrtfSetting]));
    }

    /// <summary>The game members the takeover touches, resolved from the verified catalogue.</summary>
    private sealed record Members(
        MethodInfo StartAudio,
        MethodInfo CreateAudioData,
        MethodInfo CreateAudio,
        MethodInfo CreateAudioInGame,
        MethodInfo UpdateListener,
        PropertyInfo Devices,
        PropertyInfo CurrentDevice,
        PropertyInfo MasterLevel,
        FieldInfo OpenAl,
        MethodInfo OpenAlDispose,
        MethodInfo DisposeAllSounds,
        MethodInfo ChangeOutputDevice,
        MethodInfo PlaySoundAt,
        FieldInfo PlatformInstance,
        FieldInfo IntroMusic,
        FieldInfo SoundAudioData)
    {
        public static Members Resolve(GameAssemblies game)
        {
            T Get<T>(string id)
                where T : MemberInfo =>
                PatchTargetVerifier.Resolve(game, AudioPatchTargets.Get(id)) as T
                ?? throw new InvalidOperationException($"integration point '{id}' could not be resolved");

            return new Members(
                Get<MethodInfo>("platform.start-audio"),
                Get<MethodInfo>("platform.create-audio-data"),
                Get<MethodInfo>("platform.create-audio"),
                Get<MethodInfo>("platform.create-audio-game"),
                Get<MethodInfo>("platform.update-listener"),
                Get<PropertyInfo>("platform.devices"),
                Get<PropertyInfo>("platform.current-device"),
                Get<PropertyInfo>("platform.master-level"),
                Get<FieldInfo>("platform.openal"),
                Get<MethodInfo>("openal.dispose"),
                Get<MethodInfo>("loadedsound.dispose-all"),
                Get<MethodInfo>("loadedsound.change-output-device"),
                Get<MethodInfo>("clientmain.play-sound-at"),
                Get<FieldInfo>("screenmanager.platform"),
                Get<FieldInfo>("screenmanager.intro-music"),
                Get<FieldInfo>("screenmanager.audio-data"));
        }
    }
}

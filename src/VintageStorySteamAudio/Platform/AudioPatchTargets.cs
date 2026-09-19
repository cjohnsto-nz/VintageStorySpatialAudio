namespace VintageStorySteamAudio.Platform;

/// <summary>
/// Every game member and structural fact the audio takeover depends on
/// (see docs/PLAN.md §2 and docs/investigations/phase0.md).
/// </summary>
/// <remarks>
/// This list is verified before anything is patched. If any entry fails, the mod refuses to
/// take over and the game keeps vanilla audio. When a game update changes one of these, the
/// fix is made here first, then in the patch that uses it.
/// </remarks>
public static class AudioPatchTargets
{
    private const string Platform = "Vintagestory.Client.NoObf.ClientPlatformWindows";
    private const string PlatformBase = "Vintagestory.Client.NoObf.ClientPlatformAbstract";
    private const string ClientMain = "Vintagestory.Client.NoObf.ClientMain";
    private const string LoadedSoundNative = "Vintagestory.Client.LoadedSoundNative";
    private const string AudioMetaData = "Vintagestory.Client.NoObf.AudioMetaData";
    private const string ScreenManager = "Vintagestory.Client.ScreenManager";
    private const string SystemSoundEngine = "Vintagestory.Client.NoObf.SystemSoundEngine";

    private const string Void = "System.Void";
    private const string Float = "System.Single";
    private const string Double = "System.Double";
    private const string Int = "System.Int32";
    private const string String = "System.String";
    private const string ILoadedSound = "Vintagestory.API.Client.ILoadedSound";
    private const string SoundParams = "Vintagestory.API.Client.SoundParams";
    private const string AudioData = "Vintagestory.Client.NoObf.AudioData";
    private const string IAsset = "Vintagestory.API.Common.IAsset";
    private const string AssetLocation = "Vintagestory.API.Common.AssetLocation";
    private const string EnumSoundType = "Vintagestory.API.Common.EnumSoundType";

    private static PatchTarget Method(string id, string type, string name, string returns, string[] parameters, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Method, name, returns, parameters, isStatic, purpose);

    private static PatchTarget Property(string id, string type, string name, string propertyType, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Property, name, propertyType, [], isStatic, purpose);

    private static PatchTarget Field(string id, string type, string name, string fieldType, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Field, name, fieldType, [], isStatic, purpose);

    public static IReadOnlyList<PatchTarget> Members { get; } =
    [
        // --- The platform seam: every sound is created, and the device is owned, through these.
        Method("platform.start-audio", Platform, "StartAudio", Void, [], "suppress the OpenAL device in-world; restore it on hand-back"),
        Method("platform.stop-audio", Platform, "StopAudio", Void, [], "engine shutdown ordering"),
        Method("platform.create-audio-data", Platform, "CreateAudioData", AudioData, [IAsset], "decode assets into the native asset store"),
        Method("platform.create-audio", Platform, "CreateAudio", ILoadedSound, [SoundParams, AudioData], "create engine sounds (menu/threadpool path)"),
        Method("platform.create-audio-game", Platform, "CreateAudio", ILoadedSound, [SoundParams, AudioData, ClientMain], "create engine sounds (in-game path)"),
        Method("platform.update-listener", Platform, "UpdateAudioListener", Void, [Float, Float, Float, Float, Float, Float], "listener pose (replaced by full camera basis)"),
        Method("platform.settings-watchers", Platform, "AddAudioSettingsWatchers", Void, [], "map the HRTF setting to our output mode"),
        Property("platform.devices", Platform, "AvailableAudioDevices", "System.Collections.Generic.IList<System.String>", "device list in the settings menu"),
        Property("platform.current-device", Platform, "CurrentAudioDevice", String, "device selection"),
        Property("platform.master-level", Platform, "MasterSoundLevel", Float, "master volume"),

        // --- Vanilla sound objects that exist when we take over (intro music) and hand back.
        Field("loadedsound.registry", LoadedSoundNative, "loadedSounds", "System.Collections.Generic.List<Vintagestory.Client.LoadedSoundNative>", "migrate live vanilla sounds at takeover", isStatic: true),
        Field("loadedsound.registry-lock", LoadedSoundNative, "loadedSoundsLock", "System.Object", "lock for the registry", isStatic: true),
        Method("loadedsound.dispose-all", LoadedSoundNative, "DisposeAllSounds", Void, [], "release vanilla sources before closing OpenAL", isStatic: true),
        Property("loadedsound.playback-position", LoadedSoundNative, "PlaybackPosition", Float, "resume migrated sounds at the same position"),

        // --- Asset metadata shared between the game and the engine.
        Field("audiometa.pcm", AudioMetaData, "Pcm", "System.Byte[]", "decoded PCM handed to the engine"),
        Field("audiometa.channels", AudioMetaData, "Channels", Int, "asset channel count"),
        Field("audiometa.rate", AudioMetaData, "Rate", Int, "asset sample rate"),
        Field("audiometa.bits", AudioMetaData, "BitsPerSample", Int, "asset sample format"),
        Field("audiometa.asset", AudioMetaData, "Asset", IAsset, "asset identity"),
        Method("audiometa.add-on-loaded", AudioMetaData, "AddOnLoaded", Void, ["Vintagestory.Client.NoObf.MainThreadAction"], "deferred start for still-loading assets"),

        // --- Game-side policy we replace.
        Method("clientmain.play-sound-at", ClientMain, "PlaySoundAtInternal", Int, [AssetLocation, Double, Double, Double, Float, Float, Float, EnumSoundType], "remove the fixed 250-sound cap"),
        Field("clientmain.active-sounds", ClientMain, "ActiveSounds", "System.Collections.Generic.Queue<Vintagestory.API.Client.ILoadedSound>", "the game's sound bookkeeping"),
        Method("soundengine.reverb-scan", SystemSoundEngine, "scanReverbnessOffthread", Void, [], "retire the 40-ray reverbness scan"),
        Method("soundengine.tick-100ms", SystemSoundEngine, "OnGameTick100ms", Void, [Float], "underwater / glitch / legacy reverb application"),

        // --- Main menu.
        Field("screenmanager.intro-music", ScreenManager, "IntroMusic", ILoadedSound, "the menu music playing when a world starts", isStatic: true),
        Field("screenmanager.audio-data", ScreenManager, "soundAudioData", "System.Collections.Generic.Dictionary<Vintagestory.API.Common.AssetLocation, Vintagestory.Client.NoObf.AudioData>", "asset table", isStatic: true),
    ];

    public static IReadOnlyList<GameInvariant> Invariants { get; } =
    [
        new(
            "platform.single-implementation",
            $"{Platform} is the only concrete {PlatformBase} (all audio flows through one class on every OS)",
            game =>
            {
                Type? baseType = game.FindType(PlatformBase);
                if (baseType is null) return $"{PlatformBase} not found";
                var concrete = game.LibTypes().Where(t => t is { IsAbstract: false } && baseType.IsAssignableFrom(t)).Select(t => t.FullName).ToList();
                return concrete.Count == 1 && concrete[0] == Platform
                    ? null
                    : $"expected exactly [{Platform}], found [{string.Join(", ", concrete)}]";
            }),
        new(
            "platform.audio-surface",
            $"{PlatformBase} declares no audio members beyond the ones we replace",
            game =>
            {
                Type? baseType = game.FindType(PlatformBase);
                if (baseType is null) return $"{PlatformBase} not found";
                string[] known =
                [
                    "StartAudio", "StopAudio", "CreateAudioData", "CreateAudio", "UpdateAudioListener", "AddAudioSettingsWatchers",
                    "get_AvailableAudioDevices", "get_CurrentAudioDevice", "set_CurrentAudioDevice", "get_MasterSoundLevel", "set_MasterSoundLevel",
                ];
                var unknown = baseType
                    .GetMethods(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.DeclaredOnly)
                    .Select(m => m.Name)
                    .Where(n => (n.Contains("Audio", StringComparison.OrdinalIgnoreCase) || n.Contains("Sound", StringComparison.OrdinalIgnoreCase)) && !known.Contains(n))
                    .Distinct()
                    .ToList();
                return unknown.Count == 0 ? null : $"new audio-related members: {string.Join(", ", unknown)}";
            }),
    ];
}

namespace VintageStorySpatialAudio.Platform;

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
    private const string AudioOpenAl = "Vintagestory.Client.AudioOpenAl";
    private const string AudioMetaData = "Vintagestory.Client.NoObf.AudioMetaData";
    private const string ScreenManager = "Vintagestory.Client.ScreenManager";
    private const string SystemSoundEngine = "Vintagestory.Client.NoObf.SystemSoundEngine";
    private const string AnimationManager = "Vintagestory.API.Common.AnimationManager";

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
    private const string SoundAttributes = "Vintagestory.API.Common.SoundAttributes";
    private const string Entity = "Vintagestory.API.Common.Entities.Entity";
    private const string IPlayer = "Vintagestory.API.Common.IPlayer";
    private const string Bool = "System.Boolean";

    private static PatchTarget Method(string id, string type, string name, string returns, string[] parameters, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Method, name, returns, parameters, isStatic, purpose);

    private static PatchTarget Property(string id, string type, string name, string propertyType, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Property, name, propertyType, [], isStatic, purpose);

    private static PatchTarget Field(string id, string type, string name, string fieldType, string purpose, bool isStatic = false) =>
        new(id, type, TargetMemberKind.Field, name, fieldType, [], isStatic, purpose);

    private static PatchTarget Constructor(string id, string type, string[] parameters, string purpose) =>
        new(id, type, TargetMemberKind.Constructor, ".ctor", Void, parameters, false, purpose);

    /// <summary>The target with this id.</summary>
    public static PatchTarget Get(string id) =>
        Members.FirstOrDefault(t => t.Id == id) ?? throw new ArgumentException($"no patch target '{id}'", nameof(id));

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
        Field("platform.openal", Platform, "audio", AudioOpenAl, "close OpenAL in-world (disposed and cleared; StartAudio recreates it on hand-back)"),
        Method("openal.dispose", AudioOpenAl, "Dispose", Void, [], "close the OpenAL device and context"),

        // --- Vanilla sound objects that exist when we take over (intro music) and hand back.
        Field("loadedsound.registry", LoadedSoundNative, "loadedSounds", "System.Collections.Generic.List<Vintagestory.Client.LoadedSoundNative>", "migrate live vanilla sounds at takeover", isStatic: true),
        Field("loadedsound.registry-lock", LoadedSoundNative, "loadedSoundsLock", "System.Object", "lock for the registry", isStatic: true),
        Method("loadedsound.dispose-all", LoadedSoundNative, "DisposeAllSounds", Void, [], "release vanilla sources before closing OpenAL", isStatic: true),
        Property("loadedsound.playback-position", LoadedSoundNative, "PlaybackPosition", Float, "resume migrated sounds at the same position"),
        Method("loadedsound.change-output-device", LoadedSoundNative, "ChangeOutputDevice", Void, ["System.Action"], "suppress OpenAL device/HRTF rebuilds in-world", isStatic: true),

        // --- Asset metadata shared between the game and the engine.
        Field("audiometa.pcm", AudioMetaData, "Pcm", "System.Byte[]", "decoded PCM handed to the engine"),
        Field("audiometa.channels", AudioMetaData, "Channels", Int, "asset channel count"),
        Field("audiometa.rate", AudioMetaData, "Rate", Int, "asset sample rate"),
        Field("audiometa.bits", AudioMetaData, "BitsPerSample", Int, "asset sample format"),
        Field("audiometa.asset", AudioMetaData, "Asset", IAsset, "asset identity"),
        Constructor("audiometa.ctor", AudioMetaData, [IAsset], "metadata for natively decoded assets"),
        Method("audiometa.unload", AudioMetaData, "Unload", Void, [], "make vanilla decode again after hand-back"),
        Field("audiodata.loaded", AudioData, "Loaded", Int, "loading state (0 none, 1 loading, 2 decoded, 3 in use)"),
        Method("audiodata.load", AudioData, "Load", "System.Boolean", [], "synchronous decode, as vanilla does before creating a sound"),

        // --- Game-side policy we replace.
        Method("clientmain.play-sound-at", ClientMain, "PlaySoundAtInternal", Int, [AssetLocation, Double, Double, Double, Float, Float, Float, EnumSoundType], "remove the fixed 250-sound cap; note sounds that should follow an entity"),

        // --- Sounds played at an entity: vanilla places them once; we make them follow it.
        Method("clientmain.play-sound-at-entity", ClientMain, "PlaySoundAt", Int, [SoundAttributes, Entity, IPlayer, Float], "sounds played at an entity follow it"),
        Method("clientmain.play-sound-at-entity-pitch", ClientMain, "PlaySoundAt", Void, [AssetLocation, Entity, IPlayer, Float, Float, Float], "sounds played at an entity follow it"),
        Method("clientmain.play-sound-at-entity-random-pitch", ClientMain, "PlaySoundAt", Void, [AssetLocation, Entity, IPlayer, Bool, Float, Float], "sounds played at an entity follow it"),
        Field("clientmain.active-sounds", ClientMain, "ActiveSounds", "System.Collections.Generic.Queue<Vintagestory.API.Client.ILoadedSound>", "the game's sound bookkeeping"),
        Method("soundengine.reverb-scan", SystemSoundEngine, "scanReverbnessOffthread", Void, [], "retire the 40-ray reverbness scan"),
        Method("soundengine.tick-100ms", SystemSoundEngine, "OnGameTick100ms", Void, [Float], "underwater / glitch / legacy reverb application"),

        // --- Creatures that are not being drawn: vanilla stops advancing their animations, and a
        //     creature's footsteps are triggered by animation frames (ADR 0020).
        Method("animmanager.client-frame", AnimationManager, "OnClientFrame", Void, [Float], "footsteps of a creature you cannot see"),
        Field("animmanager.entity", AnimationManager, "entity", Entity, "whose animations are being advanced"),

        // --- Main menu.
        Field("screenmanager.platform", ScreenManager, "Platform", PlatformBase, "the platform instance (hand-back calls its StartAudio)", isStatic: true),
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

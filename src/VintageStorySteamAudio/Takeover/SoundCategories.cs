using Vintagestory.API.Common;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Takeover;

/// <summary>The game's sound categories, their volume settings and our buses.</summary>
public static class SoundCategories
{
    /// <summary>Client setting (0..100) behind each bus, as vanilla's LoadedSoundNative.GlobalVolume reads them.</summary>
    public static IReadOnlyDictionary<AudioBus, string> LevelSettings { get; } = new Dictionary<AudioBus, string>
    {
        [AudioBus.Sound] = "soundLevel",
        [AudioBus.Entity] = "entitySoundLevel",
        [AudioBus.Ambient] = "ambientSoundLevel",
        [AudioBus.Weather] = "weatherSoundLevel",
        [AudioBus.Music] = "musicLevel",
    };

    public const string MasterSetting = "masterSoundLevel";
    public const string HrtfSetting = "useHRTFaudio";
    public const string DeviceSetting = "audioDevice";

    /// <summary>The bus a sound plays on; the "glitch unaffected" variants share their parent's bus.</summary>
    public static AudioBus BusFor(EnumSoundType type) => type switch
    {
        EnumSoundType.Music or EnumSoundType.MusicGlitchunaffected => AudioBus.Music,
        EnumSoundType.Ambient or EnumSoundType.AmbientGlitchunaffected => AudioBus.Ambient,
        EnumSoundType.Weather => AudioBus.Weather,
        EnumSoundType.Entity => AudioBus.Entity,
        _ => AudioBus.Sound,
    };
}

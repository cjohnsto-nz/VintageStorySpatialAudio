using System.Reflection.Emit;
using HarmonyLib;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySpatialAudio.Config;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Takeover;

namespace VintageStorySpatialAudio.Tests;

/// <summary>Takeover logic that needs neither the game nor the native engine.</summary>
public sealed class TakeoverUnitTests
{
    [Theory]
    [InlineData(EnumSoundType.Sound, AudioBus.Sound)]
    [InlineData(EnumSoundType.SoundGlitchunaffected, AudioBus.Sound)]
    [InlineData(EnumSoundType.Entity, AudioBus.Entity)]
    [InlineData(EnumSoundType.Ambient, AudioBus.Ambient)]
    [InlineData(EnumSoundType.AmbientGlitchunaffected, AudioBus.Ambient)]
    [InlineData(EnumSoundType.Weather, AudioBus.Weather)]
    [InlineData(EnumSoundType.Music, AudioBus.Music)]
    [InlineData(EnumSoundType.MusicGlitchunaffected, AudioBus.Music)]
    public void Sound_types_map_to_the_bus_of_their_volume_slider(EnumSoundType type, AudioBus bus) =>
        Assert.Equal(bus, SoundCategories.BusFor(type));

    [Fact]
    public void Every_bus_has_a_level_setting() =>
        Assert.Equal(Enum.GetValues<AudioBus>().Order(), SoundCategories.LevelSettings.Keys.Order());

    [Fact]
    public void Only_animations_that_carry_a_sound_are_worth_advancing_unseen()
    {
        static AnimationMetaData Anim(params AnimationSound[] sounds) => new() { AnimationSounds = sounds };
        static AnimationSound Sound(float range, string? location = "creature/wolf/footsteps/dirt/footstep-wolf-dirt1") =>
            new() { Attributes = new SoundAttributes { Location = location is null ? null : new AssetLocation(location), Range = range } };

        Assert.Equal(0f, AudioTakeover.FurthestAnimationSound([]));
        Assert.Equal(0f, AudioTakeover.FurthestAnimationSound(new() { ["walk"] = new AnimationMetaData() }));
        Assert.Equal(0f, AudioTakeover.FurthestAnimationSound(new() { ["walk"] = Anim(Sound(10f, location: null)) }));
        Assert.Equal(15f, AudioTakeover.FurthestAnimationSound(new()
        {
            ["walk"] = Anim(Sound(10f), Sound(10f)),
            ["run"] = Anim(Sound(15f)),
            ["idle"] = new AnimationMetaData(),
        }));
    }

    [Fact]
    public void Listener_basis_keeps_pitch_and_an_orthogonal_up_vector()
    {
        var basis = new ListenerBasis();
        Assert.True(basis.Update(0, 0, -2));
        AssertVector(0, 0, -1, basis.ForwardX, basis.ForwardY, basis.ForwardZ);
        AssertVector(0, 1, 0, basis.UpX, basis.UpY, basis.UpZ);

        // 45 degrees up, facing +x.
        Assert.True(basis.Update(1, 1, 0));
        float s = MathF.Sqrt(0.5f);
        AssertVector(s, s, 0, basis.ForwardX, basis.ForwardY, basis.ForwardZ);
        AssertVector(-s, s, 0, basis.UpX, basis.UpY, basis.UpZ);

        // Straight up: forward is +y, up points back along the last heading's opposite.
        Assert.True(basis.Update(0, 5, 0));
        AssertVector(0, 1, 0, basis.ForwardX, basis.ForwardY, basis.ForwardZ);
        AssertVector(-1, 0, 0, basis.UpX, basis.UpY, basis.UpZ);

        Assert.False(basis.Update(0, 0, 0));
        Assert.False(basis.Update(float.NaN, 0, 1));
        AssertVector(0, 1, 0, basis.ForwardX, basis.ForwardY, basis.ForwardZ);
    }

    [Fact]
    public void Placement_follows_vanilla_positioning_rules()
    {
        Assert.Equal(SpatialMode.None, SpatialAudioSound.Placement(new SoundParams { Position = null }).Mode);

        // Relative at the origin (UI, the player's own sounds): unpositioned.
        Assert.Equal(SpatialMode.None, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(), RelativePosition = true }).Mode);

        VoicePlacement relative = SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(1, 2, 3), RelativePosition = true });
        Assert.Equal(new VoicePlacement(SpatialMode.Listener, 1, 2, 3, MathF.Sqrt(32f) - 2f), relative);

        // Vanilla's reference distance: sqrt(range) - 2, at least 3; an explicit value wins.
        VoicePlacement world = SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(10, 64, -5), Range = 100 });
        Assert.Equal(new VoicePlacement(SpatialMode.World, 10, 64, -5, 8f), world);
        Assert.Equal(3f, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(1, 1, 1), Range = 16 }).MinDistance);
        Assert.Equal(1.5f, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(1, 1, 1), ReferenceDistance = 1.5f }).MinDistance);
    }

    [Fact]
    public void The_reference_distance_multiplier_carries_every_sound_further()
    {
        try
        {
            // Doubling it doubles the distance a sound plays at full volume, and the engine's
            // 1/d beyond that then gives about 6 dB more wherever you stand.
            SpatialAudioSound.ReferenceDistanceScale = 2f;
            Assert.Equal(16f, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(10, 64, -5), Range = 100 }).MinDistance);
            Assert.Equal(6f, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(1, 1, 1), Range = 16 }).MinDistance);
            // An explicit reference distance is carried too: it is the same curve either way.
            Assert.Equal(3f, SpatialAudioSound.Placement(new SoundParams { Position = new Vec3f(1, 1, 1), ReferenceDistance = 1.5f }).MinDistance);

            // At 32 m a sound with an 8 m reference is 0.25; at 16 m it is 0.5: 6 dB louder.
            Assert.Equal(6.02f, 20f * MathF.Log10(16f / 8f), 1);
        }
        finally
        {
            SpatialAudioSound.ReferenceDistanceScale = 1f;
        }
    }

    [Fact]
    public void The_sound_cap_transpiler_replaces_exactly_the_250_constant()
    {
        CodeInstruction[] original =
        [
            new(OpCodes.Ldc_I4, 100),
            new(OpCodes.Ldc_I4, 250),
            new(OpCodes.Ldc_I4, 250),
            new(OpCodes.Ret),
        ];
        CodeInstruction[] patched = [.. PlatformPatches.RemoveSoundCap(original)];

        Assert.Equal(100, patched[0].operand);
        Assert.Equal(int.MaxValue, patched[1].operand);
        Assert.Equal(250, patched[2].operand);  // only the first occurrence
        Assert.True(PlatformPatches.SoundCapRemoved);

        _ = PlatformPatches.RemoveSoundCap([new CodeInstruction(OpCodes.Ret)]).ToArray();
        Assert.False(PlatformPatches.SoundCapRemoved);
    }


    [Fact]
    public void OpenAl_device_names_from_the_settings_find_our_devices()
    {
        AudioDevice[] devices =
        [
            new("Speakers (Realtek(R) Audio)", true, new byte[512]),
            new("AV Receiver (NVIDIA High Definition Audio)", false, new byte[512]),
        ];
        Assert.Same(devices[1], AudioTakeover.MatchDevice(devices, "OpenAL Soft on AV Receiver (NVIDIA High Definition Audio)"));
        Assert.Same(devices[0], AudioTakeover.MatchDevice(devices, "speakers (realtek(r) audio)"));
        Assert.Same(devices[1], AudioTakeover.MatchDevice(devices, "AV Receiver"));
        Assert.Null(AudioTakeover.MatchDevice(devices, "Headset"));
    }

    [Fact]
    public void Category_trims_are_parsed_and_clamped()
    {
        var config = new SpatialAudioConfig
        {
            CategoryTrimDb = new() { ["Music"] = -6f, ["entity"] = 3f, ["Bogus"] = 1f, ["Weather"] = -100f, ["Sound"] = float.NaN },
        };
        IReadOnlyDictionary<AudioBus, float> trims = config.CategoryTrimsDb();
        Assert.Equal(-6f, trims[AudioBus.Music]);
        Assert.Equal(3f, trims[AudioBus.Entity]);
        Assert.Equal(-40f, trims[AudioBus.Weather]);
        Assert.False(trims.ContainsKey(AudioBus.Sound));
        Assert.Equal(3, trims.Count);
    }

    [Fact]
    public void Only_Ogg_and_RIFF_WAVE_data_is_treated_as_audio()
    {
        byte[] riff(string form) => [.. "RIFF"u8, 36, 0, 0, 0, .. System.Text.Encoding.ASCII.GetBytes(form)];
        Assert.True(AudioTakeover.LooksLikeAudio([.. "OggS"u8, 0, 2]));
        Assert.True(AudioTakeover.LooksLikeAudio(riff("WAVEfmt ")));
        Assert.False(AudioTakeover.LooksLikeAudio(riff("AVI LIST")));
        Assert.False(AudioTakeover.LooksLikeAudio("{ \"defaultBlockSounds\": {} }"u8));
        Assert.False(AudioTakeover.LooksLikeAudio([]));
    }

    private static void AssertVector(float x, float y, float z, float ax, float ay, float az)
    {
        Assert.Equal(x, ax, 4);
        Assert.Equal(y, ay, 4);
        Assert.Equal(z, az, 4);
    }
}

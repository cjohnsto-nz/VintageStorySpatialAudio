using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Takeover;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// <see cref="SpatialAudioSound"/> against the real engine, rendered offline: the ILoadedSound
/// contract vanilla code relies on.
/// </summary>
[Collection(NativeEngineGroup.Name)]
public sealed class SpatialAudioSoundTests
{
    private const int Rate = 48000;

    [Fact]
    public void A_new_sound_has_stopped_and_starts_on_request()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);

        Assert.True(sound.IsReady);
        Assert.False(sound.IsPlaying);
        Assert.True(sound.HasStopped);  // vanilla: an initial OpenAL source counts as stopped
        Assert.Equal(1, sound.Channels);
        Assert.Equal(0.5f, sound.SoundLengthSeconds, 3);

        sound.Start();
        Assert.True(sound.IsPlaying);
        Assert.False(sound.HasStopped);
        rig.Render(0.1);
        Assert.InRange(sound.PlaybackPosition, 0.09f, 0.12f);

        sound.Pause();
        Assert.True(sound.IsPaused);
        Assert.False(sound.HasStopped);
        sound.Toggle(true);
        Assert.True(sound.IsPlaying);
        sound.Toggle(false);
        Assert.True(sound.HasStopped);
        Assert.Equal(0f, sound.PlaybackPosition);
    }

    [Fact]
    public void Start_on_a_playing_sound_restarts_it_like_OpenAL()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        rig.Render(0.3);
        Assert.True(sound.PlaybackPosition > 0.25f);
        sound.Start();
        rig.Render(0.02);
        Assert.True(sound.PlaybackPosition < 0.05f);
        Assert.True(sound.IsPlaying);
    }

    [Fact]
    public void A_fade_calls_back_on_pump_and_leaves_the_volume_at_its_target()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        int calls = 0;
        sound.FadeTo(0.2, 0.1f, faded =>
        {
            Assert.Same(sound, faded);
            calls++;
        });
        Assert.True(sound.IsFadingOut);

        // Vanilla code keeps calling SetVolume during fades; that must not cancel it.
        sound.SetVolume(0.9f);
        rig.Render(0.2);
        Assert.Equal(0, calls);  // callbacks run on the main thread, from Pump
        rig.Session.Pump();
        Assert.Equal(1, calls);
        Assert.False(sound.IsFadingOut);
        Assert.Equal(0.2f, sound.Params.Volume, 3);
    }

    [Fact]
    public void Only_the_latest_fade_calls_back_and_fade_targets_are_clamped()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        var called = new List<string>();
        sound.FadeTo(0.5, 5f, _ => called.Add("first"));
        rig.Render(0.05);
        sound.FadeTo(0.3, 0.05f, _ => called.Add("second"));  // replaces the first
        rig.Render(0.2);
        rig.Session.Pump();
        Assert.Equal(["second"], called);
        Assert.Equal(0.3f, sound.Params.Volume, 4);

        sound.FadeIn(0.05f, _ => called.Add("in"));
        Assert.True(sound.IsFadingIn);
        rig.Render(0.2);
        rig.Session.Pump();
        Assert.Equal(["second", "in"], called);
        Assert.Equal(1f, sound.Params.Volume, 4);

        sound.FadeOut(0.05f, _ => called.Add("out"));
        rig.Render(0.2);
        rig.Session.Pump();
        Assert.Equal(0.01f, sound.Params.Volume, 4);  // vanilla fades never go below 0.01
        Assert.True(sound.IsPlaying);                  // FadeOut alone does not stop
    }

    [Fact]
    public void FadeOutAndStop_stops_once_the_fade_completes()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        sound.FadeOutAndStop(0.05f);
        rig.Render(0.2);
        Assert.True(sound.IsPlaying);
        rig.Session.Pump();
        Assert.True(sound.HasStopped);
    }

    [Fact]
    public void A_sound_whose_data_is_still_loading_plays_once_it_arrives()
    {
        using var rig = new Rig();
        AudioAsset? ready = null;
        var soundParams = new SoundParams(new AssetLocation("game:sounds/test.ogg")) { Position = null };
        SpatialAudioSound sound = rig.Session.CreateSound(soundParams, () => ready, 1);
        Assert.False(sound.IsReady);
        Assert.Equal(1, rig.Session.PendingCount);

        sound.Start();
        sound.PlaybackPosition = 0.25f;
        sound.SetVolume(0.5f);
        Assert.False(sound.IsPlaying);  // as vanilla: no source yet
        rig.Session.Pump();
        Assert.False(sound.IsReady);

        ready = rig.Asset;
        rig.Session.Pump();
        Assert.True(sound.IsReady);
        Assert.True(sound.IsPlaying);
        Assert.InRange(sound.PlaybackPosition, 0.24f, 0.26f);
        Assert.Equal(0, rig.Session.PendingCount);
    }

    [Fact]
    public void Disposed_sounds_report_stopped_and_ignore_everything()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        sound.Dispose();
        sound.Dispose();
        Assert.True(sound.IsDisposed);
        Assert.False(sound.IsPlaying);
        Assert.True(sound.HasStopped);
        sound.Start();
        sound.SetVolume(1f);
        sound.SetPosition(1, 2, 3);
        sound.FadeTo(1, 1, _ => Assert.Fail("disposed sounds do not fade"));
        rig.Render(0.05);
        rig.Session.Pump();
        Assert.Equal(0, rig.Session.SoundCount);
    }

    [Fact]
    public void Sounds_outliving_the_engine_are_inert_not_fatal()
    {
        var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        sound.Start();
        rig.Dispose();  // hand-back: engine gone while game code still holds the sound

        Assert.False(sound.IsPlaying);
        sound.Start();
        sound.Stop();
        sound.SetPitch(1.2f);
        sound.SetLowPassfiltering(0.06f);
        sound.PlaybackPosition = 0.1f;
        _ = sound.PlaybackPosition;
        sound.Dispose();
    }

    [Fact]
    public void Giving_an_unpositioned_sound_a_position_makes_it_a_world_sound()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true);
        Assert.Null(sound.Params.Position);
        sound.SetPosition(new Vec3f(3, 4, 5));
        Assert.Equal(new Vec3f(3, 4, 5), sound.Params.Position);
        Assert.False(sound.Params.RelativePosition);

        sound.Start();
        rig.Render(0.1);
        Assert.Equal(1, rig.Session.Engine.GetStats().RealVoices);
        Assert.True(sound.HasReverbStopped(0));
    }

    [Fact]
    public void Levels_become_bus_and_master_gains()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(loop: true, type: EnumSoundType.Music);
        sound.Start();
        rig.Session.ApplyLevels(new AudioLevels(100, AllBuses(100), Headphones: true));
        double full = rig.RenderRms(0.1);
        rig.Session.ApplyLevels(new AudioLevels(100, AllBuses(100, music: 50), Headphones: true));
        rig.Render(0.02);
        double half = rig.RenderRms(0.1);
        Assert.True(Math.Abs((half / full) - 0.5) < 0.01, $"half/full = {half / full}, full = {full}");
        rig.Session.ApplyLevels(new AudioLevels(50, AllBuses(100, music: 50), Headphones: false));
        rig.Render(0.02);
        Assert.True(Math.Abs((rig.RenderRms(0.1) / full) - 0.25) < 0.01);
    }

    [Fact]
    public void The_listener_offset_puts_the_players_own_sounds_in_front()
    {
        using var rig = new Rig();
        rig.Session.Engine.OpenOffline(Rate, 6);
        rig.Session.Engine.SetRenderMode(RenderMode.Speakers);
        rig.Session.ListenerBackwardOffset = 0.5f;
        rig.Session.SetListener(100, 70, 100, 0, 0, -1);  // eyes at y 70, facing -z (north)

        // A footstep straight below the eyes: without the offset its direction is purely vertical,
        // and the tiniest horizontal error flips it between the front and rear speakers.
        SpatialAudioSound step = rig.Session.CreateSound(
            new SoundParams { Location = new AssetLocation("game:sounds/tone.ogg"), ShouldLoop = true, Position = new Vec3f(100, 68.4f, 100) },
            () => rig.Asset,
            1);
        step.Start();
        rig.Render6(0.1);
        float[] output = rig.Render6(0.2);
        double front = Power(output, 6, 0) + Power(output, 6, 1) + Power(output, 6, 2);
        double rear = Power(output, 6, 4) + Power(output, 6, 5);
        Assert.True(front > rear * 4, $"front {front}, rear {rear}");
    }

    [Fact]
    public void Positions_are_sent_relative_to_the_origin_and_follow_it_when_it_moves()
    {
        using var rig = new Rig();
        rig.Session.Engine.OpenOffline(Rate, 6);
        rig.Session.Engine.SetRenderMode(RenderMode.Speakers);
        rig.Session.SetOrigin(512000, 64, 512000);
        rig.Session.SetListener(512010, 70, 512010, 0, 0, -1);  // facing -z

        // A sound 3 m to the listener's left, at Vintage Story's usual far-from-zero coordinates.
        SpatialAudioSound sound = rig.Session.CreateSound(
            new SoundParams { Location = new AssetLocation("game:sounds/tone.ogg"), ShouldLoop = true, Position = new Vec3f(512007, 70, 512010) },
            () => rig.Asset,
            1);
        sound.Start();
        rig.Render6(0.1);
        float[] left = rig.Render6(0.2);
        Assert.True(Power(left, 6, 0) > Power(left, 6, 1) * 10, "the sound is on the left");

        // Moving the origin re-sends the listener and the sound: still on the left.
        rig.Session.SetOrigin(511000, 0, 513000);
        rig.Render6(0.1);
        float[] after = rig.Render6(0.2);
        Assert.True(Power(after, 6, 0) > Power(after, 6, 1) * 10, "still on the left after the origin moved");
        Assert.Equal(new SceneOrigin(511000, 0, 513000), rig.Session.Origin);
        Assert.Equal((511000, 0, 513000), rig.Session.Engine.GetSceneStats().Origin);
    }

    [Fact]
    public void The_session_places_sounds_and_listener_in_the_simulation_frame_with_the_eyes_as_listener()
    {
        using var rig = new Rig();
        rig.Session.Engine.SetSceneMaterials(World.MaterialTable.Build(WorldSceneTests.ShippedConfig()).Materials);
        rig.Session.SetOrigin(512000, 64, 512000);
        var snapshot = new ChunkSnapshot { X = 16000, Y = 2, Z = 16000 };
        for (int y = 0; y < 32; y++)
        {
            for (int z = 0; z < 32; z++)
            {
                snapshot.Materials[(y * 32 + z) * 32 + 10] = 3;
            }
        }

        rig.Session.Engine.SetSceneChunk(snapshot);
        Assert.True(rig.Session.Engine.WaitSceneIdle(TimeSpan.FromSeconds(10)));
        rig.Session.ListenerBackwardOffset = 0.5f;
        rig.Session.SetListener(512004, 80.5f, 512016.5f, 1, 0, 0);
        SpatialAudioSound sound = rig.Session.CreateSound(
            new SoundParams { Location = new AssetLocation("game:sounds/tone.ogg"), ShouldLoop = true, Position = new Vec3f(512020, 80.5f, 512016.5f) },
            () => rig.Asset,
            1);
        sound.Start();
        rig.Render(0.3);
        SourceDebugInfo s = Assert.Single(rig.Session.Engine.GetSimulatedSources());
        Assert.Equal((20f, 16.5f, 16.5f), s.Position);
        Assert.True(s.Occlusion < 0.05f);
        Assert.Equal(1, s.Crossings);
        SimulationStats sim = rig.Session.Engine.GetSimulationStats();
        Assert.Equal((512000, 64, 512000), sim.Origin);
        Assert.Equal(4f, sim.Listener.X, 3);  // the eyes, not 0.5 m behind them
    }

    [Fact]
    public void A_surround_weather_bed_plays_each_channel_from_its_speaker()
    {
        // As vanilla loads weather tracks (relative, at the origin): with a 5.1 asset, a bed.
        // Signal in the left surround only (WAV/PCM order FL FR FC LFE BL BR).
        using var rig = new Rig();
        short[] pcm = new short[Rate * 6];
        short[] tone = NativeEngineTests.Sine(1000, Rate, Rate, 0.5);
        for (int j = 0; j < Rate; j++)
        {
            pcm[(j * 6) + 4] = tone[j];
        }

        using AudioAsset bed = rig.Session.Engine.CreatePcmAsset(pcm, 6, Rate, "rain-5.1");
        foreach (RenderMode mode in new[] { RenderMode.Speakers, RenderMode.Headphones })
        {
            rig.Session.Engine.SetRenderMode(mode);
            var soundParams = new SoundParams
            {
                Location = new AssetLocation("game:sounds/weather/tracks/rain-leafless.ogg"),
                Position = new Vec3f(0, 0, 0),
                RelativePosition = true,
                ShouldLoop = true,
                SoundType = EnumSoundType.Weather,
            };
            SpatialAudioSound sound = rig.Session.CreateSound(soundParams, () => bed, 6);
            Assert.Equal(6, sound.Channels);
            sound.Start();
            rig.Render(0.1);
            float[] out2 = new float[Rate / 2 * 2];
            rig.Session.Engine.RenderOffline(out2);
            Assert.True(Power(out2, 2, 0) > Power(out2, 2, 1) * 4, $"{mode}: the left surround is heard on the left");
            sound.Dispose();
            rig.Render(0.2);
        }
    }

    private static double Power(float[] interleaved, int channels, int channel)
    {
        double sum = 0;
        for (int i = channel; i < interleaved.Length; i += channels)
        {
            sum += interleaved[i] * (double)interleaved[i];
        }

        return sum;
    }

    private static Dictionary<AudioBus, int> AllBuses(int level, int? music = null) =>
        Enum.GetValues<AudioBus>().ToDictionary(b => b, b => b == AudioBus.Music && music is int m ? m : level);

    /// <summary>An engine on the offline output, a session and a 0.5 s tone asset.</summary>
    private sealed class Rig : IDisposable
    {
        private readonly AudioEngine engine;

        public Rig()
        {
            NativeTestEnvironment.RequireNatives();
            engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam }, null);
            Session = new AudioSession(engine, null);
            Asset = engine.CreatePcmAsset(NativeEngineTests.Sine(480, Rate, Rate / 2, 0.5), 1, Rate, "tone");
        }

        public AudioSession Session { get; }

        public AudioAsset Asset { get; }

        public SpatialAudioSound Sound(bool loop = false, EnumSoundType type = EnumSoundType.Sound) =>
            Session.CreateSound(new SoundParams { Location = new AssetLocation("game:sounds/tone.ogg"), ShouldLoop = loop, SoundType = type }, () => Asset, 1);

        public void Render(double seconds) => engine.RenderOffline(new float[(int)(seconds * Rate) * 2]);

        public float[] Render6(double seconds)
        {
            float[] output = new float[(int)(seconds * Rate) * 6];
            engine.RenderOffline(output);
            return output;
        }

        public double RenderRms(double seconds)
        {
            float[] output = new float[(int)(seconds * Rate) * 2];
            engine.RenderOffline(output);
            return Math.Sqrt(output.Sum(x => (double)x * x) / output.Length);
        }

        public void Dispose()
        {
            Session.Dispose();
            Asset.Dispose();
            engine.Dispose();
        }
    }
}

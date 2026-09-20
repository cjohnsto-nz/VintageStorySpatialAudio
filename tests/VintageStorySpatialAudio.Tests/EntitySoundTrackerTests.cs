using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.Common.Entities;
using Vintagestory.API.MathTools;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Takeover;
using Body = VintageStorySpatialAudio.Takeover.EntitySoundInference.Body;

namespace VintageStorySpatialAudio.Tests;

/// <summary>Sounds following the entity that made them: announcement, matching and inference.</summary>
[Collection(NativeEngineGroup.Name)]
public sealed class EntitySoundTrackerTests
{
    [Fact]
    public void An_announced_sound_is_claimed_once_by_its_exact_position()
    {
        var tracker = new EntitySoundTracker();
        var anchor = new FakeAnchor();
        tracker.Expect(512000.5f, 110.9f, 511999.25f, anchor, nowMs: 0);

        Assert.Null(tracker.Claim(World(512000.5f, 110.9f, 511999.3f), 1));  // not quite the same place
        Assert.Null(tracker.Claim(new SoundParams { Position = new Vec3f(512000.5f, 110.9f, 511999.25f), RelativePosition = true }, 1));
        Assert.Same(anchor, tracker.Claim(World(512000.5f, 110.9f, 511999.25f), 1));
        Assert.Null(tracker.Claim(World(512000.5f, 110.9f, 511999.25f), 1));
        Assert.Equal(0, tracker.ExpectedCount);
    }

    [Fact]
    public void An_announcement_waits_for_a_late_decode_but_not_forever()
    {
        var tracker = new EntitySoundTracker();
        tracker.Expect(1, 2, 3, new FakeAnchor(), nowMs: 1000);
        tracker.Expect(4, 5, 6, new FakeAnchor(), nowMs: 1000);

        Assert.NotNull(tracker.Claim(World(1, 2, 3), 1000 + EntitySoundTracker.ExpectMs - 1));
        tracker.Update(1000 + EntitySoundTracker.ExpectMs);
        Assert.Equal(0, tracker.ExpectedCount);
        Assert.Null(tracker.Claim(World(4, 5, 6), 1000 + EntitySoundTracker.ExpectMs));
    }

    [Fact]
    public void A_skipped_sound_withdraws_its_announcement()
    {
        var tracker = new EntitySoundTracker();
        tracker.Expect(1, 2, 3, new FakeAnchor(), nowMs: 0);
        tracker.Cancel(1, 2, 3);
        Assert.Null(tracker.Claim(World(1, 2, 3), 0));
    }

    [Fact]
    public void The_entity_context_nests_and_is_restored()
    {
        Entity outer = new EntityChunky();
        Entity inner = new EntityChunky();
        PlatformPatches.PlaySoundAtEntity(outer, out Entity? outerState);
        PlatformPatches.PlaySoundAtEntity(inner, out Entity? innerState);
        Assert.Same(inner, PlatformPatches.EmittingEntity);
        PlatformPatches.PlaySoundAtEntityDone(innerState);
        Assert.Same(outer, PlatformPatches.EmittingEntity);
        PlatformPatches.PlaySoundAtEntityDone(outerState);
        Assert.Null(PlatformPatches.EmittingEntity);
    }

    [Fact]
    public void The_players_own_sounds_are_head_locked_ahead_and_a_little_below()
    {
        var soundParams = new SoundParams { Position = new Vec3f(512000.5f, 110.9f, 511999.25f), Range = 16 };
        OwnBodyAnchor.Place(soundParams);
        VoicePlacement placement = SpatialAudioSound.Placement(soundParams, new SceneOrigin(512000, 100, 512000));

        // Listener space, -z ahead: well in front, below ear level but far from the nadir, and far
        // enough from the head to keep its direction.
        Assert.Equal(SpatialMode.Listener, placement.Mode);
        Assert.Equal(0f, placement.X);
        float distance = MathF.Sqrt((placement.Y * placement.Y) + (placement.Z * placement.Z));
        float elevation = MathF.Asin(placement.Y / distance) * 180f / MathF.PI;
        Assert.True(placement.Z < 0);
        Assert.InRange(elevation, -25f, -10f);
        Assert.InRange(distance, 0.6f, 1f);
        Assert.False(OwnBodyAnchor.Instance.TryGetPosition(out _, out _, out _));
    }

    [Theory]
    [InlineData("sounds/creature/wolf/growl", EnumSoundType.Sound, true)]
    [InlineData("sounds/voice/saxophone", EnumSoundType.Sound, true)]
    [InlineData("sounds/effect/anvilhit", EnumSoundType.Entity, true)]
    [InlineData("sounds/block/planks", EnumSoundType.Sound, false)]
    [InlineData("sounds/player/build", EnumSoundType.Sound, false)]
    public void Only_creature_sounds_are_matched_by_position(string path, EnumSoundType type, bool eligible) =>
        Assert.Equal(eligible, EntitySoundInference.IsEligible(new AssetLocation("game", path), type));

    [Fact]
    public void A_sound_halfway_up_a_tall_creature_is_matched_to_it()
    {
        // A polar bear's selection box is 2.9 tall: the server sends its calls from 1.45 above its feet.
        Body bear = new(100, 50, 100, Height: 2.9, Radius: 1.05);
        Body hare = new(106, 50, 100, Height: 0.5, Radius: 0.25);
        Assert.Equal(0, EntitySoundInference.Pick(100, 51.45, 100, [bear, hare], tolerance: 1));
        Assert.Equal(0.0, EntitySoundInference.Gap(100.5, 51.45, 100, bear));
    }

    [Fact]
    public void A_running_creature_lagging_on_the_client_is_still_matched()
    {
        // The client's interpolated position trails the server's by up to a block for a sprinting wolf.
        Body wolf = new(100, 50, 100, Height: 1.1, Radius: 0.6);
        Assert.Equal(0, EntitySoundInference.Pick(101.4, 50.55, 100, [wolf], tolerance: 1));
        Assert.Equal(-1, EntitySoundInference.Pick(102.7, 50.55, 100, [wolf], tolerance: 1));
    }

    [Fact]
    public void Two_creatures_equally_close_match_neither()
    {
        Body a = new(100, 50, 100, Height: 0.7, Radius: 0.3);
        Body b = new(100.8, 50, 100, Height: 0.7, Radius: 0.3);
        Assert.Equal(-1, EntitySoundInference.Pick(100.4, 50.35, 100, [a, b], tolerance: 1));

        // Clearly nearer one of them: fine.
        Body far = new(102, 50, 100, Height: 0.7, Radius: 0.3);
        Assert.Equal(0, EntitySoundInference.Pick(100.1, 50.35, 100, [a, far], tolerance: 1));
        Assert.Equal(-1, EntitySoundInference.Pick(100, 50.35, 100, [], tolerance: 1));
    }

    [Fact]
    public void A_tracked_sound_follows_its_anchor_until_the_entity_is_gone()
    {
        using var rig = new Rig();
        SpatialAudioSound sound = rig.Sound(new Vec3f(10, 20, 30));
        sound.Start();
        var anchor = new FakeAnchor { X = 10, Y = 20, Z = 30 };
        var tracker = new EntitySoundTracker();
        tracker.Track(sound, anchor);

        anchor.X = 12.5;
        anchor.Z = 29;
        tracker.Update(0);
        AssertPosition(12.5f, 20, 29, sound);
        Assert.Equal(1, tracker.Count);

        // Gone (despawned): the sound stays where it last was and is no longer tracked.
        anchor.Gone = true;
        tracker.Update(0);
        Assert.Equal(0, tracker.Count);
        AssertPosition(12.5f, 20, 29, sound);
        Assert.True(sound.IsPlaying);
    }

    [Fact]
    public void A_stopped_or_disposed_sound_is_let_go()
    {
        using var rig = new Rig();
        SpatialAudioSound stopped = rig.Sound(new Vec3f(0, 1, 0));
        SpatialAudioSound disposed = rig.Sound(new Vec3f(0, 1, 0));
        SpatialAudioSound playing = rig.Sound(new Vec3f(0, 1, 0));
        stopped.Start();
        stopped.Stop();
        disposed.Start();
        disposed.Dispose();
        playing.Start();
        var tracker = new EntitySoundTracker();
        tracker.Track(stopped, new FakeAnchor());
        tracker.Track(disposed, new FakeAnchor());
        tracker.Track(playing, new FakeAnchor { Inferred = true });

        tracker.Update(0);
        Assert.Equal(1, tracker.Count);
        Assert.Equal(1, tracker.InferredCount);
    }

    private static SoundParams World(float x, float y, float z) => new() { Position = new Vec3f(x, y, z) };

    private static void AssertPosition(float x, float y, float z, SpatialAudioSound sound)
    {
        Vec3f p = sound.Params.Position;
        Assert.Equal(x, p.X, 3);
        Assert.Equal(y, p.Y, 3);
        Assert.Equal(z, p.Z, 3);
    }

    private sealed class FakeAnchor : ISoundAnchor
    {
        public double X { get; set; }

        public double Y { get; set; }

        public double Z { get; set; }

        public bool Gone { get; set; }

        public bool Inferred { get; set; }

        public bool TryGetPosition(out double x, out double y, out double z)
        {
            (x, y, z) = (X, Y, Z);
            return !Gone;
        }
    }

    private sealed class Rig : IDisposable
    {
        private const int Rate = 48000;
        private readonly AudioEngine engine;
        private readonly AudioSession session;
        private readonly AudioAsset asset;

        public Rig()
        {
            NativeTestEnvironment.RequireNatives();
            engine = AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam }, null);
            session = new AudioSession(engine, null);
            asset = engine.CreatePcmAsset(NativeEngineTests.Sine(480, Rate, Rate / 2, 0.5), 1, Rate, "tone");
        }

        public SpatialAudioSound Sound(Vec3f position) =>
            session.CreateSound(new SoundParams { Location = new AssetLocation("game:sounds/tone.ogg"), Position = position, ShouldLoop = true }, () => asset, 1);

        public void Dispose()
        {
            session.Dispose();
            asset.Dispose();
            engine.Dispose();
        }
    }
}

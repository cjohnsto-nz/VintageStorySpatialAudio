using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.Common.Entities;
using Vintagestory.API.MathTools;

namespace VintageStorySteamAudio.Takeover;

/// <summary>Where a tracked sound is now.</summary>
public interface ISoundAnchor
{
    /// <summary>Matched to its entity by position (the game gave coordinates only), not named by the game.</summary>
    bool Inferred { get; }

    /// <summary>False once the entity is gone; the sound then stays where it last was.</summary>
    bool TryGetPosition(out double x, out double y, out double z);
}

/// <summary>
/// Makes sounds played at an entity follow it while they play. Vanilla places them once, where the
/// entity was, so a running wolf's growl stays behind it.
/// <para>
/// PlaySoundAtInternal announces each such sound with <see cref="Expect"/>, and the platform's
/// CreateAudio claims the announcement by the sound's exact position (<see cref="Claim"/>). They
/// are usually the same call, but the first play of an asset decodes on the thread pool first and
/// creates the sound later.
/// </para>
/// Only PlaySoundAt's one-shots are tracked. Sounds that entity code loads itself (a mount's gait,
/// bees, bells) are moved by their owners, and we don't fight them.
/// </summary>
/// <remarks>Thread-safe; <see cref="Update"/> runs once per frame on the main thread.</remarks>
public sealed class EntitySoundTracker
{
    /// <summary>How long an announcement waits for its sound: longer than any first decode.</summary>
    internal const long ExpectMs = 10_000;

    /// <summary>Movement smaller than this (metres) is not sent to the engine.</summary>
    private const double MinMove = 0.01;

    private readonly Lock gate = new();
    private readonly List<Expected> expected = [];
    private readonly List<Tracked> tracked = [];

    /// <summary>Sounds following an entity now.</summary>
    public int Count
    {
        get
        {
            lock (gate)
            {
                return tracked.Count;
            }
        }
    }

    /// <summary>Of those, how many were matched to their entity by position.</summary>
    public int InferredCount
    {
        get
        {
            lock (gate)
            {
                return tracked.Count(t => t.Anchor.Inferred);
            }
        }
    }

    /// <summary>Announcements not yet claimed by a sound.</summary>
    public int ExpectedCount
    {
        get
        {
            lock (gate)
            {
                return expected.Count;
            }
        }
    }

    /// <summary>A sound for <paramref name="anchor"/> is being played at this position (as the game stores it: floats).</summary>
    public void Expect(float x, float y, float z, ISoundAnchor anchor, long nowMs)
    {
        ArgumentNullException.ThrowIfNull(anchor);
        lock (gate)
        {
            expected.Add(new Expected(x, y, z, anchor, nowMs + ExpectMs));
        }
    }

    /// <summary>The game played nothing at this position after all (out of range, missing asset).</summary>
    public void Cancel(float x, float y, float z)
    {
        lock (gate)
        {
            int i = Find(x, y, z);
            if (i >= 0)
            {
                expected.RemoveAt(i);
            }
        }
    }

    /// <summary>The anchor announced for a sound being created, or null if it should stay put.</summary>
    public ISoundAnchor? Claim(SoundParams soundParams, long nowMs)
    {
        ArgumentNullException.ThrowIfNull(soundParams);
        if (soundParams.Position is not { } p || soundParams.RelativePosition)
        {
            return null;
        }

        lock (gate)
        {
            if (expected.Count == 0)
            {
                return null;
            }

            expected.RemoveAll(e => e.ExpiresMs <= nowMs);
            int i = Find(p.X, p.Y, p.Z);
            if (i < 0)
            {
                return null;
            }

            ISoundAnchor anchor = expected[i].Anchor;
            expected.RemoveAt(i);
            return anchor;
        }
    }

    /// <summary>From now on <paramref name="sound"/> follows <paramref name="anchor"/>, until it stops.</summary>
    public void Track(ILoadedSound sound, ISoundAnchor anchor)
    {
        ArgumentNullException.ThrowIfNull(sound);
        ArgumentNullException.ThrowIfNull(anchor);
        Vec3f? p = sound.Params?.Position;
        lock (gate)
        {
            tracked.Add(new Tracked(sound, anchor) { X = p?.X ?? double.NaN, Y = p?.Y ?? double.NaN, Z = p?.Z ?? double.NaN });
        }
    }

    /// <summary>Main thread, once per frame: moves every tracked sound to its entity; lets go of ended ones.</summary>
    public void Update(long nowMs)
    {
        lock (gate)
        {
            if (expected.Count > 0)
            {
                expected.RemoveAll(e => e.ExpiresMs <= nowMs);
            }

            for (int i = tracked.Count - 1; i >= 0; i--)
            {
                Tracked t = tracked[i];
                // A sound still waiting for its data reports stopped; it has not ended.
                if (t.Sound.IsDisposed || (t.Sound.IsReady && t.Sound.HasStopped) || !t.Anchor.TryGetPosition(out double x, out double y, out double z))
                {
                    tracked.RemoveAt(i);
                    continue;
                }

                double dx = x - t.X;
                double dy = y - t.Y;
                double dz = z - t.Z;
                if ((dx * dx) + (dy * dy) + (dz * dz) < MinMove * MinMove)  // false for NaN (none sent yet)
                {
                    continue;
                }

                t.X = x;
                t.Y = y;
                t.Z = z;
                t.Sound.SetPosition((float)x, (float)y, (float)z);
            }
        }
    }

    /// <summary>Forgets everything (hand-back).</summary>
    public void Clear()
    {
        lock (gate)
        {
            expected.Clear();
            tracked.Clear();
        }
    }

    /// <summary>The newest announcement at exactly this position, or -1.</summary>
    private int Find(float x, float y, float z)
    {
        for (int i = expected.Count - 1; i >= 0; i--)
        {
            Expected e = expected[i];
            if (e.X == x && e.Y == y && e.Z == z)
            {
                return i;
            }
        }

        return -1;
    }

    private readonly record struct Expected(float X, float Y, float Z, ISoundAnchor Anchor, long ExpiresMs);

    private sealed class Tracked(ILoadedSound sound, ISoundAnchor anchor)
    {
        public ILoadedSound Sound { get; } = sound;

        public ISoundAnchor Anchor { get; } = anchor;

        /// <summary>The position last sent.</summary>
        public double X { get; set; }

        public double Y { get; set; }

        public double Z { get; set; }
    }
}

/// <summary>
/// The listening player's own sounds (armour, eating, tools): head-locked a little ahead of and
/// below the ears, like vanilla's own footsteps (played at the head).
/// <para>
/// Their true place, the middle of the body, is almost straight below the ears. Panning puts that
/// mostly on the nadir, which is shared by every ear-level speaker, so they came from the rears
/// too (a third of their power looking ahead, more looking up, as the listener tilts with the
/// camera).
/// </para>
/// </summary>
public sealed class OwnBodyAnchor : ISoundAnchor
{
    public static readonly OwnBodyAnchor Instance = new();

    private OwnBodyAnchor()
    {
    }

    /// <summary>Listener space (+x right, +y up, -z ahead), metres: 18° below straight ahead.</summary>
    public static Vec3f Position => new(0f, -0.25f, -0.75f);

    public bool Inferred => false;

    /// <summary>Never tracked: the sound is head-locked instead.</summary>
    public bool TryGetPosition(out double x, out double y, out double z)
    {
        x = y = z = 0;
        return false;
    }

    /// <summary>Makes a sound head-locked at <see cref="Position"/>.</summary>
    public static void Place(SoundParams soundParams)
    {
        ArgumentNullException.ThrowIfNull(soundParams);
        soundParams.Position = Position;
        soundParams.RelativePosition = true;
    }
}

/// <summary>A sound's place on an entity: the entity's position plus a fixed offset.</summary>
internal sealed class EntityAnchor : ISoundAnchor
{
    private readonly IClientWorldAccessor world;
    private readonly Entity entity;
    private readonly double offsetX;
    private readonly double offsetY;
    private readonly double offsetZ;

    private EntityAnchor(IClientWorldAccessor world, Entity entity, double offsetX, double offsetY, double offsetZ, bool inferred)
    {
        this.world = world;
        this.entity = entity;
        this.offsetX = offsetX;
        this.offsetY = offsetY;
        this.offsetZ = offsetZ;
        Inferred = inferred;
    }

    public bool Inferred { get; }

    /// <summary>
    /// The game played the sound at this entity (at <paramref name="x"/>, <paramref name="y"/>,
    /// <paramref name="z"/>, just computed from its position): keep that offset, halfway up its body.
    /// </summary>
    public static EntityAnchor? Named(IClientWorldAccessor world, Entity entity, double x, double y, double z) =>
        entity.Pos is { } pos ? new EntityAnchor(world, entity, x - pos.X, y - pos.InternalY, z - pos.Z, inferred: false) : null;

    /// <summary>
    /// Matched by position: keep only the height on its body. The horizontal offset is mostly the
    /// client's interpolation lag behind the server, and keeping it would put the sound ahead of a
    /// running creature.
    /// </summary>
    public static EntityAnchor? Matched(IClientWorldAccessor world, Entity entity, double y) =>
        entity.Pos is { } pos
            ? new EntityAnchor(world, entity, 0, Math.Clamp(y - pos.InternalY, 0, EntitySoundInference.BodyOf(entity).Height), 0, inferred: true)
            : null;

    public bool TryGetPosition(out double x, out double y, out double z)
    {
        EntityPos? pos = entity.Pos;
        if (pos is null || !world.LoadedEntities.TryGetValue(entity.EntityId, out Entity? loaded) || !ReferenceEquals(loaded, entity))
        {
            x = y = z = 0;
            return false;
        }

        x = pos.X + offsetX;
        y = pos.InternalY + offsetY;
        z = pos.Z + offsetZ;
        return true;
    }
}

/// <summary>
/// Which creature a sound given only coordinates belongs to. The server sends creature sounds as
/// plain positions (halfway up the creature when it was sent), even in single player.
/// </summary>
public static class EntitySoundInference
{
    /// <summary>A second candidate this much further (blocks) than the best makes the match certain enough.</summary>
    public const double AmbiguityMargin = 0.5;

    /// <summary>An entity's body: an upright axis from its feet, and how far its sides reach from it.</summary>
    public readonly record struct Body(double X, double Y, double Z, double Height, double Radius);

    /// <summary>Sounds a creature makes: the Entity category, and creature and voice assets.</summary>
    public static bool IsEligible(AssetLocation? location, EnumSoundType soundType)
    {
        if (soundType == EnumSoundType.Entity)
        {
            return true;
        }

        string? path = location?.Path;
        return path is not null
            && (path.Contains("creature/", StringComparison.OrdinalIgnoreCase) || path.Contains("voice/", StringComparison.OrdinalIgnoreCase));
    }

    public static Body BodyOf(Entity entity)
    {
        ArgumentNullException.ThrowIfNull(entity);
        EntityPos pos = entity.Pos;
        if (entity.SelectionBox is { } box)
        {
            return new Body(pos.X, pos.InternalY, pos.Z, Math.Max(0, box.Y2), Math.Max(0, (box.X2 - box.X1) / 2));
        }

        Vec2f? size = entity.Properties?.CollisionBoxSize;
        return new Body(pos.X, pos.InternalY, pos.Z, size?.Y ?? 1, (size?.X ?? 0) / 2);
    }

    /// <summary>
    /// The body a sound at (<paramref name="x"/>, <paramref name="y"/>, <paramref name="z"/>) came
    /// from: the nearest within <paramref name="tolerance"/> blocks of its surface, unless another is
    /// nearly as near. -1 if none, or if it is ambiguous.
    /// </summary>
    public static int Pick(double x, double y, double z, IReadOnlyList<Body> bodies, double tolerance)
    {
        ArgumentNullException.ThrowIfNull(bodies);
        int best = -1;
        double bestGap = double.MaxValue;
        double secondGap = double.MaxValue;
        for (int i = 0; i < bodies.Count; i++)
        {
            double gap = Gap(x, y, z, bodies[i]);
            if (gap > tolerance)
            {
                continue;
            }

            if (gap < bestGap)
            {
                secondGap = bestGap;
                bestGap = gap;
                best = i;
            }
            else if (gap < secondGap)
            {
                secondGap = gap;
            }
        }

        return best >= 0 && secondGap - bestGap >= AmbiguityMargin ? best : -1;
    }

    /// <summary>Distance from a point to a body's surface (0 inside it).</summary>
    public static double Gap(double x, double y, double z, Body body)
    {
        double dx = x - body.X;
        double dz = z - body.Z;
        double top = body.Y + body.Height;
        double dy = y < body.Y ? body.Y - y : y > top ? y - top : 0;
        double horizontal = Math.Max(0, Math.Sqrt((dx * dx) + (dz * dz)) - body.Radius);
        return Math.Sqrt((horizontal * horizontal) + (dy * dy));
    }
}

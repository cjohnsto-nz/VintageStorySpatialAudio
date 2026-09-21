using System.Globalization;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;

namespace VintageStorySpatialAudio.Diagnostics;

/// <summary>
/// Plays a game sound at a place in the world through the game's own <c>PlaySoundAt</c>, rather
/// than straight through the engine as <see cref="TestPlayback"/> does. Everything a real sound
/// meets is then in the way: the range check, the distance fall-off, occlusion, reflections and
/// pathing. For asking how far away something can be heard, and from where.
/// </summary>
internal static class WorldSoundTest
{
    /// <summary>Vanilla's default range for <c>PlaySoundAt</c>, which a caller that says nothing gets.</summary>
    public const float DefaultRange = 32f;

    /// <summary>
    /// Plays <paramref name="name"/> (as the game names sounds: <c>creature/wolf/howl1</c>, or
    /// <c>creature/wolf/howl*</c> to pick one at random) at <paramref name="position"/>.
    /// </summary>
    /// <param name="api">The client API.</param>
    /// <param name="name">The sound, as the game names them.</param>
    /// <param name="position">Where in the world to play it.</param>
    /// <param name="range">The sound's range, as a caller of PlaySoundAt would give it.</param>
    /// <param name="volume">Volume multiplier.</param>
    /// <param name="pitch">Pitch multiplier.</param>
    /// <param name="rangeScale">
    /// What the game's range check is multiplied by: <c>SoundRangeMultiplier</c> under the
    /// takeover, 1 without it.
    /// </param>
    public static string PlayAt(ICoreClientAPI api, string name, Vec3d position, float range, float volume, float pitch, float rangeScale)
    {
        ArgumentNullException.ThrowIfNull(api);
        ArgumentNullException.ThrowIfNull(position);
        var location = new AssetLocation(name.Trim());

        // A wildcard is the game's to resolve; anything else we can say is missing before it plays
        // silently (PlaySoundAt returns nothing to tell us with).
        if (!location.Path.Contains('*', StringComparison.Ordinal) && api.Assets.TryGet(TestPlayback.ToSoundLocation(name)) is null)
        {
            return $"Sound asset '{TestPlayback.ToSoundLocation(name)}' not found.";
        }

        Vec3d? listener = api.World?.Player?.Entity?.Pos?.XYZ;
        api.World?.PlaySoundAt(location, position.X, position.Y, position.Z, null, EnumSoundType.Entity, pitch, range, volume);

        string where = string.Create(
            CultureInfo.InvariantCulture,
            $"Played {location} at ={position.X:0.#} ={position.Y:0.#} ={position.Z:0.#} (Entity, range {range:0.#}, volume {volume:0.##}, pitch {pitch:0.##}).");
        return listener is null ? where : where + "\n" + Describe(listener.DistanceTo(position), range, rangeScale);
    }

    /// <summary>
    /// Whether the game will start a sound of this range at this distance, and out to what
    /// distance it would. Being started is not being heard: past its reference distance a sound
    /// goes on fading, which is the question this command is for.
    /// </summary>
    internal static string Describe(double distance, float range, float rangeScale)
    {
        double cutoff = (double)range * (float.IsFinite(rangeScale) && rangeScale > 0f ? rangeScale : 1f);
        string scale = Math.Abs(rangeScale - 1f) < 1e-6f
            ? "the game's own range"
            : string.Create(CultureInfo.InvariantCulture, $"range {range:0.#} x SoundRangeMultiplier {rangeScale:0.##}");
        string verdict = distance <= cutoff ? "Started." : "Too far: nothing played.";
        return string.Create(
            CultureInfo.InvariantCulture,
            $"{distance:0.0} m from the listener; sounds of this range are started out to {cutoff:0.0} m ({scale}). {verdict}");
    }
}

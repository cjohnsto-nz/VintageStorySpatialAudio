using System.Globalization;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.Common.Entities;
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
        AssetLocation location = ToGameSoundLocation(name);

        // A wildcard is the game's to resolve; anything else we can say is missing up front.
        if (!location.Path.Contains('*', StringComparison.Ordinal) && api.Assets.TryGet(TestPlayback.ToSoundLocation(name)) is null)
        {
            return $"Sound asset '{TestPlayback.ToSoundLocation(name)}' not found.";
        }

        var sound = new SoundAttributes
        {
            Location = location,
            Range = range,
            Type = EnumSoundType.Entity,
            Pitch = NatFloat.createUniform(pitch, 0f),
        };

        EntityPos? listener = api.World?.Player?.Entity?.Pos;
        int played = api.World?.PlaySoundAt(sound, position.X, position.Y, position.Z, listener?.Dimension ?? 0, null, volume) ?? 0;

        string where = string.Create(
            CultureInfo.InvariantCulture,
            $"{location} at {position.X:0.#} {position.Y:0.#} {position.Z:0.#} (Entity, range {range:0.#}, volume {volume:0.##}, pitch {pitch:0.##}).");
        double distance = listener is null ? double.NaN : listener.XYZ.DistanceTo(position);
        return where + "\n" + Describe(distance, range, rangeScale, played);
    }

    /// <summary>
    /// What the game did with it, and out to what distance it would have started it. Being
    /// started is not being heard: past its reference distance a sound goes on fading, which is
    /// the question this command is for.
    /// </summary>
    /// <param name="distance">Metres from the listener; NaN when there is no listener.</param>
    /// <param name="range">The range the sound was given.</param>
    /// <param name="rangeScale">What the game's range check is multiplied by.</param>
    /// <param name="played">
    /// What PlaySoundAt returned: 0 for nothing played, otherwise the length or a pending-load
    /// marker. It is the game's own answer, so it catches the reasons distance cannot explain.
    /// </param>
    internal static string Describe(double distance, float range, float rangeScale, int played)
    {
        double cutoff = (double)range * (float.IsFinite(rangeScale) && rangeScale > 0f ? rangeScale : 1f);
        string scale = Math.Abs(rangeScale - 1f) < 1e-6f
            ? "the game's own range"
            : string.Create(CultureInfo.InvariantCulture, $"range {range:0.#} x SoundRangeMultiplier {rangeScale:0.##}");
        string verdict = played > 0
            ? "The game started it."
            : distance > cutoff
                ? "The game played nothing: past the range check."
                : "The game played nothing, though it is in range. Look in client-main.log for "
                  + "'Audio File not found', and check the sound is not muted in the settings.";
        string away = double.IsNaN(distance) ? "?" : distance.ToString("0.0", CultureInfo.InvariantCulture);
        return string.Create(
            CultureInfo.InvariantCulture,
            $"{away} m from the listener; sounds of this range are started out to {cutoff:0.0} m ({scale}). {verdict}");
    }

    /// <summary>
    /// The location as the game's <c>PlaySoundAt</c> wants it: under <c>sounds/</c>, which the
    /// caller is expected to have added (vanilla's JSON loader does, in
    /// <c>AnimationSound.OnDeserialized</c>) and which <c>ResolveSoundPath</c> does not add; and
    /// without the extension, which the game appends once any wildcard has been resolved.
    /// </summary>
    internal static AssetLocation ToGameSoundLocation(string name)
    {
        var location = new AssetLocation(name.Trim());
        return location.Path.StartsWith("sounds/", StringComparison.Ordinal)
            ? location
            : new AssetLocation(location.Domain, "sounds/" + location.Path);
    }
}

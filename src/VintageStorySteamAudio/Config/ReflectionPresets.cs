using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Config;

/// <summary>Named qualities for the reflections (PLAN 5.4). Medium is the engine's own default.</summary>
public enum ReflectionQuality
{
    Low,
    Medium,
    High,
    Ultra,
}

/// <summary>
/// The reflection presets. What costs is not the rays but rebuilding every source's impulse
/// response (about 4 ms per source per second of response at order 2): the presets mostly trade
/// how many voices get reflections of their own, and how long and how detailed those are.
/// Measured on a 12-thread desktop (core/test_render_budget.cpp): Medium runs a simulation in
/// ~40 ms on two threads, and its reflections keep the render thread under 25 % at p99.
/// </summary>
public static class ReflectionPresets
{
    public static ReflectionQualitySettings For(ReflectionQuality quality) => quality switch
    {
        ReflectionQuality.Low => new ReflectionQualitySettings
        {
            Sources = 4, Rays = 1024, Bounces = 8, DurationSeconds = 1.0f, Order = 1, RateHz = 5, TransitionSeconds = 0.08f,
        },
        ReflectionQuality.High => new ReflectionQualitySettings
        {
            Sources = 16, Rays = 4096, Bounces = 24, DurationSeconds = 1.5f, Order = 2, RateHz = 10, TransitionSeconds = 0.15f,
        },
        ReflectionQuality.Ultra => new ReflectionQualitySettings
        {
            Sources = 32, Rays = 8192, Bounces = 32, DurationSeconds = 2.0f, Order = 2, RateHz = 15, TransitionSeconds = 0.2f,
            Threads = 4,
        },
        _ => new ReflectionQualitySettings
        {
            Sources = 8, Rays = 2048, Bounces = 16, DurationSeconds = 1.0f, Order = 2, RateHz = 10, TransitionSeconds = 0.1f,
        },
    };

    /// <summary>
    /// The preset with any non-zero override in its place, each clamped to what the engine
    /// accepts (the transition kept below the duration).
    /// </summary>
    public static ReflectionQualitySettings Resolve(ReflectionQuality quality, ReflectionQualitySettings overrides)
    {
        ArgumentNullException.ThrowIfNull(overrides);
        ReflectionQualitySettings p = For(quality);
        static int Pick(int value, int preset, int min, int max) => value > 0 ? Math.Clamp(value, min, max) : preset;
        static float PickF(float value, float preset, float min, float max) =>
            float.IsFinite(value) && value > 0f ? Math.Clamp(value, min, max) : preset;
        float duration = PickF(overrides.DurationSeconds, p.DurationSeconds, 0.25f, 4f);
        return new ReflectionQualitySettings
        {
            Sources = Pick(overrides.Sources, p.Sources, 1, 64),
            Rays = Pick(overrides.Rays, p.Rays, 256, 32768),
            Bounces = Pick(overrides.Bounces, p.Bounces, 1, 64),
            DurationSeconds = duration,
            Order = Pick(overrides.Order, p.Order, 1, 3),
            RateHz = Pick(overrides.RateHz, p.RateHz, 1, 60),
            Threads = Pick(overrides.Threads, p.Threads, 1, 32),
            TransitionSeconds = Math.Min(PickF(overrides.TransitionSeconds, p.TransitionSeconds, 0.02f, 0.5f), duration * 0.9f),
        };
    }
}

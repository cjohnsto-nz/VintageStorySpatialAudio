namespace VintageStorySteamAudio.Native;

/// <summary>
/// What the engine runs as when nothing is set: every value resolved, the ones that depend on
/// the machine included. Read from the native library so the defaults live in one place, and
/// used to write the settings file with real numbers rather than zeros (ADR 0018).
/// </summary>
public sealed record EngineDefaults(
    int BlockFrames,
    int MaxVoices,
    int MaxRealVoices,
    int MaxBinauralVoices,
    int OcclusionSamples,
    int OcclusionRateHz,
    int ReflectionSources,
    int ReflectionRays,
    int ReflectionBounces,
    float ReflectionDurationSeconds,
    int ReflectionOrder,
    int ReflectionRateHz,
    int ReflectionThreads,
    float ReflectionTransitionSeconds,
    int PathingRangeBlocks,
    int PathingHeightBlocks,
    float PathingProbeSpacing,
    int PathingVisibilitySamples,
    int PathingRateHz,
    int PathingSources,
    int PathingMaxProbes)
{
    /// <summary>
    /// Asks the native library. No engine instance is needed, but the library must already be
    /// resolvable (<see cref="NativeLibraryResolver.Register"/>, which the mod does at startup).
    /// </summary>
    public static unsafe EngineDefaults Read()
    {
        var c = new VsaEngineConfig { StructSize = (uint)sizeof(VsaEngineConfig) };
        NativeException.ThrowIfFailed(VsaNative.GetDefaultConfig(ref c), "vsa_get_default_config");
        return new EngineDefaults(
            (int)c.BlockFrames, (int)c.MaxVoices, (int)c.MaxRealVoices, (int)c.MaxBinauralVoices,
            (int)c.OcclusionSamples, (int)c.DirectRateHz,
            (int)c.ReflectionSources, (int)c.ReflectionRays, (int)c.ReflectionBounces, c.ReflectionDuration,
            (int)c.ReflectionOrder, (int)c.ReflectionRateHz, (int)c.ReflectionThreads, c.ReflectionTransition,
            (int)c.PathingRange, (int)c.PathingHeight, c.PathingProbeSpacing, (int)c.PathingVisSamples,
            (int)c.PathingRateHz, (int)c.PathingSources, (int)c.PathingMaxProbes);
    }
}

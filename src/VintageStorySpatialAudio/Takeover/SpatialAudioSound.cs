using Vintagestory.API.Client;
using Vintagestory.API.MathTools;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Diagnostics;

namespace VintageStorySpatialAudio.Takeover;

/// <summary>
/// The game's <see cref="ILoadedSound"/>, played by the native engine. Mirrors the contract of
/// vanilla's LoadedSoundNative (see docs/PLAN.md §2.2), not its OpenAL internals:
/// <list type="bullet">
///   <item>gain is Params.Volume (the category level is the bus gain); pitch is Pitch + offset, clamped 0.1..3;</item>
///   <item>Start on a playing sound restarts it (OpenAL semantics); HasStopped is true before the first Start;</item>
///   <item>FadeTo clamps to 0.01..1 and at least 0.02 s, is linear in dB, and calls back on the main
///         thread only if no later fade replaced it; SetVolume during a fade does not cancel it;</item>
///   <item>a sound whose data is still loading remembers what it was told and plays once it is ready.</item>
/// </list>
/// Thread-safe (the game fades from its thread pool). Engine failures never reach game code: they
/// are logged by the session and the sound keeps working as far as it can.
/// </summary>
public sealed class SpatialAudioSound : ILoadedSound
{
    private const float VanillaDefaultReferenceDistance = 3f;

    /// <summary>
    /// Every sound's reference distance is multiplied by this (ReferenceDistanceMultiplier in
    /// the settings). The fall-off itself stays physical -- the engine plays a sound at full
    /// volume within its reference distance and at 1/d beyond it -- so this moves the whole
    /// curve outwards rather than bending it: 2 is about 6 dB more at every distance past the
    /// reference, and twice the distance for a given loudness.
    /// </summary>
    public static float ReferenceDistanceScale { get; set; } = 1f;

    /// <summary>
    /// How much of a sound still reaches the listener whatever is in the way, by asset path
    /// (ADR 0022). Set from the config at takeover; <see cref="OcclusionFloors.None"/> by default.
    /// </summary>
    public static OcclusionFloors OcclusionFloors { get; set; } = OcclusionFloors.None;

    /// <summary>
    /// The sounds that are high-passed, and where (<c>HighPassHzBySound</c>). Set from the config
    /// at takeover; <see cref="HighPassFilters.None"/> by default.
    /// </summary>
    public static HighPassFilters HighPassFilters { get; set; } = HighPassFilters.None;

    private readonly AudioSession session;
    private readonly Func<AudioAsset?> resolveAsset;
    private readonly Lock gate = new();

    private Voice? voice;
    private volatile bool disposed;
    private float pitchOffset;
    private int channels;
    private float soundLength;

    private int fadeState;  // 0 none, 1 in, 2 out
    private ulong fadeToken;
    private float fadeTarget;
    private Action<ILoadedSound>? onFaded;

    // What the game asked for before the asset was ready.
    private VoiceState pendingState = VoiceState.Stopped;
    private float? pendingPosition;
    private (float Target, float Seconds)? pendingFade;

    internal SpatialAudioSound(AudioSession session, SoundParams soundParams, Func<AudioAsset?> resolveAsset, int channelsHint)
    {
        this.session = session;
        this.resolveAsset = resolveAsset;
        Params = soundParams;
        channels = channelsHint;
    }

    public SoundParams Params { get; }

    public bool IsDisposed => disposed;

    public bool IsReady => voice is not null;

    public int Channels => channels;

    public float SoundLengthSeconds => soundLength;

    public bool IsPlaying => !disposed && State == VoiceState.Playing;

    public bool IsPaused => !disposed && State == VoiceState.Paused;

    public bool HasStopped => disposed || State == VoiceState.Stopped;

    public bool IsFadingIn => Volatile.Read(ref fadeState) == 1;

    public bool IsFadingOut => Volatile.Read(ref fadeState) == 2;

    public float PlaybackPosition
    {
        get
        {
            Voice? v = voice;
            if (v is null)
            {
                return pendingPosition ?? 0f;
            }

            return session.Guard(() => (float)v.Status.PositionSeconds, 0f);
        }

        set
        {
            lock (gate)
            {
                if (voice is null)
                {
                    pendingPosition = value;
                    return;
                }

                Voice v = voice;
                session.Guard(() => v.Seek(Math.Max(0f, value)));
            }
        }
    }

    /// <summary>The native voice, once the asset is ready.</summary>
    internal ulong VoiceHandle => voice?.Handle ?? 0;

    private VoiceState State
    {
        get
        {
            Voice? v = voice;
            return v is null ? VoiceState.Stopped : session.Guard(() => v.Status.State, VoiceState.Stopped);
        }
    }

    private float EffectivePitch => Math.Clamp(Params.Pitch + pitchOffset, 0.1f, 3f);

    public void Start()
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            if (voice is null)
            {
                pendingState = VoiceState.Playing;
                return;
            }

            Voice v = voice;
            session.Guard(() =>
            {
                if (v.Status.State == VoiceState.Playing)
                {
                    v.Stop();  // OpenAL restarts a playing source from the beginning
                }

                v.Start();
            });
        }
    }

    public void Stop() => Transport(VoiceState.Stopped, v => v.Stop());

    public void Pause() => Transport(VoiceState.Paused, v => v.Pause());

    public void Toggle(bool on)
    {
        if (on)
        {
            Start();
        }
        else
        {
            Stop();
        }
    }

    public void SetPitch(float val)
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            Params.Pitch = val;
            ApplyPitch();
        }
    }

    public void SetPitchOffset(float val)
    {
        lock (gate)
        {
            pitchOffset = val;
            ApplyPitch();
        }
    }

    public void SetVolume(float val)
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            Params.Volume = val;  // SoundParams clamps to 0..1
            ApplyVolume();
        }
    }

    public void SetVolume()
    {
        lock (gate)
        {
            ApplyVolume();
        }
    }

    public void SetPosition(Vec3f position)
    {
        ArgumentNullException.ThrowIfNull(position);
        SetPosition(position.X, position.Y, position.Z);
    }

    public void SetPosition(float x, float y, float z)
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            if (Params.Position is null)
            {
                // As vanilla: giving an unpositioned sound a position makes it a world sound.
                Params.Position = new Vec3f(x, y, z);
                Params.RelativePosition = false;
            }
            else
            {
                Params.Position.Set(x, y, z);
            }

            if (voice is not null)
            {
                Voice v = voice;
                VoicePlacement placement = Placement(Params, session.Origin);
                session.Guard(() => v.SetPosition(placement.Mode, placement.X, placement.Y, placement.Z));
            }
        }
    }

    /// <summary>Re-sends a world sound's position (after the session's origin moved).</summary>
    internal void Reposition()
    {
        lock (gate)
        {
            if (voice is not null && Params.Position is not null && !Params.RelativePosition)
            {
                Voice v = voice;
                VoicePlacement placement = Placement(Params, session.Origin);
                session.Guard(() => v.SetPosition(placement.Mode, placement.X, placement.Y, placement.Z));
            }
        }
    }

    public void SetLooping(bool on)
    {
        lock (gate)
        {
            Params.ShouldLoop = on;
            if (voice is not null)
            {
                Voice v = voice;
                session.Guard(() => v.SetLooping(on));
            }
        }
    }

    public void FadeTo(double newVolume, float duration, Action<ILoadedSound> onFaded)
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            float seconds = Math.Max(0.02f, duration);
            float target = (float)Math.Clamp(newVolume, 0.01, 1.0);
            float current = Params.Volume;
            fadeState = target > current ? 1 : target < current ? 2 : 0;
            fadeTarget = target;
            this.onFaded = onFaded;
            fadeToken++;
            if (voice is null)
            {
                pendingFade = (target, seconds);
                return;
            }

            Voice v = voice;
            ulong token = fadeToken;
            session.Guard(() => v.FadeTo(target, seconds, token));
        }
    }

    public void FadeOut(float seconds, Action<ILoadedSound> onFadedOut) => FadeTo(0.0, seconds, onFadedOut);

    public void FadeIn(float seconds, Action<ILoadedSound> onFadedIn) => FadeTo(1.0, seconds, onFadedIn);

    public void FadeOutAndStop(float seconds) => FadeTo(0.0, seconds, sound => sound.Stop());

    public void SetLowPassfiltering(float value)
    {
        lock (gate)
        {
            Params.LowPassFilter = value;
            if (voice is not null)
            {
                Voice v = voice;
                float gain = Math.Clamp(value, 0f, 1f);
                session.Guard(() => v.SetLowpass(gain));
            }
        }
    }

    /// <summary>Recorded only: reverb is simulated (Phase 6), not picked from vanilla's presets.</summary>
    public void SetReverb(float reverbDecayTime) => Params.ReverbDecayTime = reverbDecayTime;

    /// <summary>There is no separate reverb tail to wait for.</summary>
    public bool HasReverbStopped(long elapsedMilliseconds) => true;

    public void Dispose()
    {
        Voice? released;
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            released = voice;
            voice = null;
            onFaded = null;
        }

        session.Unregister(this, released?.Handle ?? 0);
        released?.Dispose();
    }

    /// <summary>
    /// Creates the voice once the asset is ready and applies everything asked for meanwhile.
    /// Returns true when done (or no longer needed), false while the asset is still loading.
    /// </summary>
    internal bool TryActivate()
    {
        lock (gate)
        {
            if (disposed || voice is not null)
            {
                return true;
            }

            AudioAsset? asset;
            try
            {
                asset = resolveAsset();
            }
            catch (Exception ex) when (ex is NativeException or NotSupportedException or InvalidOperationException or ObjectDisposedException)
            {
                session.ReportFailure($"could not load {Params.Location}", ex);
                disposed = true;  // as vanilla: a sound that cannot load is disposed
                return true;
            }

            if (asset is null)
            {
                return false;
            }

            AssetInfo info = asset.Info;
            channels = info.Channels;
            soundLength = (float)info.DurationSeconds;
            try
            {
                voice = session.Engine.CreateVoice(
                    asset, SoundCategories.BusFor(Params.SoundType), Params.Volume, EffectivePitch, Params.ShouldLoop, Placement(Params, session.Origin));
            }
            catch (Exception ex) when (ex is NativeException or ObjectDisposedException)
            {
                session.ReportFailure($"could not create a voice for {Params.Location}", ex);
                disposed = true;
                return true;
            }

            session.Activated(this, voice.Handle);
            Voice v = voice;
            session.Guard(() =>
            {
                if (!session.Passes(Params.Location?.ToShortString()))
                {
                    v.SetMuted(true);  // debugging: another sound is soloed
                }

                if (Params.LowPassFilter < 1f)
                {
                    v.SetLowpass(Math.Clamp(Params.LowPassFilter, 0f, 1f));
                }

                if (pendingPosition is float position)
                {
                    v.Seek(Math.Max(0f, position));
                }

                if (pendingFade is (float target, float seconds))
                {
                    v.FadeTo(target, seconds, fadeToken);
                }

                if (pendingState == VoiceState.Playing)
                {
                    v.Start();
                }
                else if (pendingState == VoiceState.Paused)
                {
                    v.Start();
                    v.Pause();
                }
            });
            pendingPosition = null;
            pendingFade = null;
            return true;
        }
    }

    /// <summary>Main thread: a fade finished (or was replaced by another).</summary>
    internal void OnFadeDone(ulong token, bool cancelled)
    {
        Action<ILoadedSound>? callback;
        lock (gate)
        {
            if (token != fadeToken || disposed)
            {
                return;  // superseded by a later fade
            }

            fadeState = 0;
            if (cancelled)
            {
                return;
            }

            Params.Volume = fadeTarget;
            callback = onFaded;
            onFaded = null;
        }

        callback?.Invoke(this);
    }

    /// <summary>How the game's sound parameters map to engine positioning (origin at 0, 0, 0).</summary>
    public static VoicePlacement Placement(SoundParams soundParams) => Placement(soundParams, SceneOrigin.Zero);

    /// <summary>How the game's sound parameters map to engine positioning; world positions relative to <paramref name="origin"/>.</summary>
    public static VoicePlacement Placement(SoundParams soundParams, SceneOrigin origin)
    {
        ArgumentNullException.ThrowIfNull(origin);
        ArgumentNullException.ThrowIfNull(soundParams);
        Vec3f? position = soundParams.Position;
        if (position is null)
        {
            return new VoicePlacement(SpatialMode.None, HighPassHz: HighPassFilters.For(soundParams.Location?.ToString()));
        }

        // Vanilla's reference distance: an explicit one, else sqrt(range) - 2 but at least 3 m.
        float minDistance = soundParams.ReferenceDistance != VanillaDefaultReferenceDistance
            ? soundParams.ReferenceDistance
            : Math.Max(VanillaDefaultReferenceDistance, MathF.Sqrt(soundParams.Range) - 2f);
        minDistance *= ReferenceDistanceScale;
        // Head-locked sounds are the player's own and never occluded; only world sounds take one.
        float floor = OcclusionFloors.For(soundParams.Location?.ToString());
        float highPass = HighPassFilters.For(soundParams.Location?.ToString());  // wherever it plays from

        if (soundParams.RelativePosition)
        {
            // Head-locked. At the origin (the usual case: UI, player sounds) it is simply unpositioned.
            return position.X == 0f && position.Y == 0f && position.Z == 0f
                ? new VoicePlacement(SpatialMode.None, HighPassHz: highPass)
                : new VoicePlacement(SpatialMode.Listener, position.X, position.Y, position.Z, minDistance, HighPassHz: highPass);
        }

        return new VoicePlacement(
            SpatialMode.World,
            (float)((double)position.X - origin.X),
            (float)((double)position.Y - origin.Y),
            (float)((double)position.Z - origin.Z),
            minDistance,
            floor,
            highPass);
    }

    private void Transport(VoiceState pending, Action<Voice> command)
    {
        using PerfMonitor.Scope perf = PerfMonitor.Instance.Measure(PerfSection.SoundApi);
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            if (voice is null)
            {
                pendingState = pending == VoiceState.Paused && pendingState != VoiceState.Playing ? pendingState : pending;
                return;
            }

            Voice v = voice;
            session.Guard(() => command(v));
        }
    }

    private void ApplyPitch()
    {
        if (voice is not null)
        {
            Voice v = voice;
            float pitch = EffectivePitch;
            session.Guard(() => v.SetPitch(pitch));
        }
    }

    /// <summary>Debugging: silences or restores this sound under the session's solo.</summary>
    internal void ApplySolo()
    {
        lock (gate)
        {
            if (voice is null || disposed)
            {
                return;
            }

            Voice v = voice;
            bool muted = !session.Passes(Params.Location?.ToShortString());
            session.Guard(() => v.SetMuted(muted));
        }
    }

    private void ApplyVolume()
    {
        // During a fade the fade owns the gain, as vanilla's fade loop overwrites SetVolume every 10 ms.
        if (voice is not null && fadeState == 0)
        {
            Voice v = voice;
            float gain = Params.Volume;
            session.Guard(() => v.SetGain(gain));
        }
    }
}

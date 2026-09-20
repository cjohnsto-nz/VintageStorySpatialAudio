using System.Runtime.CompilerServices;

namespace VintageStorySteamAudio.Native;

public readonly record struct VoiceStatus(VoiceState State, double PositionSeconds);

/// <summary>
/// One playing (or stopped) instance of an asset. Thread-safe; calls post commands to the render
/// thread and return at once. <see cref="Status"/> reflects commands immediately, even before the
/// render thread applies them. Dispose releases the voice (with a short declick fade if audible).
/// </summary>
public sealed class Voice : IDisposable
{
    private readonly AudioEngine engine;
    private int disposed;

    internal Voice(AudioEngine engine, ulong handle)
    {
        this.engine = engine;
        Handle = handle;
    }

    /// <summary>The native handle; matches <see cref="EngineEvent.Voice"/>.</summary>
    public ulong Handle { get; }

    public VoiceStatus Status
    {
        get
        {
            using AudioEngine.Lease lease = engine.Acquire();
            var status = new VsaVoiceStatus { StructSize = (uint)Unsafe.SizeOf<VsaVoiceStatus>() };
            NativeException.ThrowIfFailed(VsaNative.VoiceGetStatus(lease.Engine, Handle, ref status), "vsa_voice_get_status");
            return new VoiceStatus((VoiceState)status.State, status.PositionSeconds);
        }
    }

    public void Start() => Call(VsaNative.VoiceStart, "vsa_voice_start");

    public void Pause() => Call(VsaNative.VoicePause, "vsa_voice_pause");

    public void Stop() => Call(VsaNative.VoiceStop, "vsa_voice_stop");

    /// <summary>Linear gain, smoothed over a few milliseconds. Cancels a running fade.</summary>
    public void SetGain(float gain)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetGain(lease.Engine, Handle, gain), "vsa_voice_set_gain");
    }

    public void SetPitch(float pitch)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetPitch(lease.Engine, Handle, pitch), "vsa_voice_set_pitch");
    }

    public void SetLooping(bool looping)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetLooping(lease.Engine, Handle, looping ? 1u : 0u), "vsa_voice_set_looping");
    }

    public void SetPosition(SpatialMode mode, float x, float y, float z)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetPosition(lease.Engine, Handle, (uint)mode, x, y, z), "vsa_voice_set_position");
    }

    /// <summary>High-frequency damping (0..1 gain above ~5 kHz, as OpenAL's EFX low-pass); 1 = off.</summary>
    /// <summary>
    /// Debugging: silences the voice whatever its gain and fades say. A silenced voice asks for
    /// no simulation, so with every other voice silenced the overlays draw this one's alone.
    /// </summary>
    public void SetMuted(bool muted)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetMuted(lease.Engine, Handle, muted ? 1u : 0u), "vsa_voice_set_muted");
    }

    public void SetLowpass(float gainHf)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSetLowpass(lease.Engine, Handle, gainHf), "vsa_voice_set_lowpass");
    }

    public void Seek(double positionSeconds)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(VsaNative.VoiceSeek(lease.Engine, Handle, positionSeconds), "vsa_voice_seek");
    }

    /// <summary>
    /// Fades the gain to <paramref name="target"/> linearly in dB; posts
    /// <see cref="EngineEventType.FadeDone"/> with <paramref name="token"/> when done.
    /// </summary>
    public void FadeTo(float target, float seconds, ulong token = 0, bool stopWhenDone = false)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        uint flags = stopWhenDone ? VsaNative.FadeStopWhenDone : 0;
        NativeException.ThrowIfFailed(VsaNative.VoiceFade(lease.Engine, Handle, target, seconds, flags, token), "vsa_voice_fade");
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0 || engine.IsDisposed)
        {
            return; // an engine that is gone has freed every voice already
        }

        try
        {
            using AudioEngine.Lease lease = engine.Acquire();
            VsaNative.VoiceRelease(lease.Engine, Handle);
        }
        catch (ObjectDisposedException)
        {
            // The engine was disposed concurrently; nothing left to release.
        }
    }

    private void Call(Func<nint, ulong, VsaResult> function, string name)
    {
        using AudioEngine.Lease lease = engine.Acquire();
        NativeException.ThrowIfFailed(function(lease.Engine, Handle), name);
    }
}

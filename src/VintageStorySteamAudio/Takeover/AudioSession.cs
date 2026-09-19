using System.Globalization;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Takeover;

/// <summary>Volume and output settings the session mirrors from the game.</summary>
/// <param name="Master">masterSoundLevel, 0..100.</param>
/// <param name="Levels">Each bus's category level, 0..100.</param>
/// <param name="Headphones">useHRTFaudio: binaural rendering instead of speaker panning.</param>
public sealed record AudioLevels(int Master, IReadOnlyDictionary<AudioBus, int> Levels, bool Headphones);

/// <summary>
/// Everything the engine plays during one world session: the sounds the game created, their
/// assets, settings and the listener. Created at takeover, disposed at hand-back.
/// </summary>
/// <remarks>
/// Threading: sounds may be created and controlled from any thread. <see cref="Pump"/> must run on
/// the game's main thread (it raises fade callbacks, which vanilla raises there too).
/// </remarks>
public sealed class AudioSession : IDisposable
{
    private readonly ILogger? logger;
    private readonly IReadOnlyDictionary<AudioBus, float> trimDb;
    private readonly Lock gate = new();
    private readonly Dictionary<ulong, SteamAudioSound> byVoice = [];
    private readonly List<SteamAudioSound> pending = [];
    private readonly EngineEvent[] events = new EngineEvent[256];
    private readonly ListenerBasis basis = new();
    private volatile SceneOrigin origin = SceneOrigin.Zero;
    private (float X, float Y, float Z, float ViewX, float ViewY, float ViewZ)? lastListener;
    private AudioLevels? lastLevels;
    private long lastFailureLog;
    private int suppressedFailures;
    private bool disposed;

    public AudioSession(AudioEngine engine, ILogger? logger, IReadOnlyDictionary<AudioBus, float>? trimDb = null)
    {
        Engine = engine ?? throw new ArgumentNullException(nameof(engine));
        this.logger = logger;
        this.trimDb = trimDb ?? new Dictionary<AudioBus, float>();
        Assets = new SoundAssetStore(engine);
    }

    public AudioEngine Engine { get; }

    public SoundAssetStore Assets { get; }

    /// <summary>Sounds with a voice, plus those still waiting for their asset.</summary>
    public int SoundCount
    {
        get
        {
            lock (gate)
            {
                return byVoice.Count + pending.Count;
            }
        }
    }

    public int PendingCount
    {
        get
        {
            lock (gate)
            {
                return pending.Count;
            }
        }
    }

    /// <summary>A game sound. <paramref name="resolveAsset"/> returns null while the asset is still loading.</summary>
    public SteamAudioSound CreateSound(SoundParams soundParams, Func<AudioAsset?> resolveAsset, int channelsHint = 1)
    {
        var sound = new SteamAudioSound(this, soundParams, resolveAsset, channelsHint);
        if (!sound.TryActivate())
        {
            lock (gate)
            {
                pending.Add(sound);
            }
        }

        return sound;
    }

    /// <summary>
    /// Main thread, once per frame: raises fade callbacks and activates sounds whose asset finished
    /// loading.
    /// </summary>
    public void Pump()
    {
        if (disposed)
        {
            return;
        }

        int count;
        while ((count = Guard(() => Engine.PollEvents(events), 0)) > 0)
        {
            for (int i = 0; i < count; i++)
            {
                EngineEvent e = events[i];
                if (e.Type != EngineEventType.FadeDone)
                {
                    if (e.Type is EngineEventType.DeviceLost or EngineEventType.DeviceRestored or EngineEventType.DeviceRerouted)
                    {
                        logger?.Notification("output device: {0}", e.Type);
                    }

                    continue;
                }

                SteamAudioSound? sound;
                lock (gate)
                {
                    byVoice.TryGetValue(e.Voice, out sound);
                }

                sound?.OnFadeDone(e.Token, e.FadeCancelled);
            }
        }

        SteamAudioSound[] waiting;
        lock (gate)
        {
            if (pending.Count == 0)
            {
                return;
            }

            waiting = [.. pending];
        }

        foreach (SteamAudioSound sound in waiting)
        {
            if (sound.TryActivate())
            {
                lock (gate)
                {
                    pending.Remove(sound);
                }
            }
        }
    }

    /// <summary>
    /// Metres the listener sits behind the eye, horizontally. The player's own sounds (footsteps,
    /// blocks at their feet) are almost straight below the ears, where a few centimetres decide
    /// between the front and rear speakers; moving the listener back puts them clearly in front.
    /// From Chris's VintageStorySurroundSound, where the same fix settled it.
    /// </summary>
    public float ListenerBackwardOffset { get; set; }

    /// <summary>
    /// The block position every position sent to the engine is relative to (the world scene's
    /// origin, ADR 0007): Vintage Story's coordinates are ~500 000, where floats are coarse.
    /// </summary>
    public SceneOrigin Origin => origin;

    /// <summary>
    /// Moves the origin: tells the engine, then re-sends the listener and every positioned sound
    /// relative to the new one.
    /// </summary>
    public void SetOrigin(int x, int y, int z)
    {
        var next = new SceneOrigin(x, y, z);
        if (next == origin)
        {
            return;
        }

        origin = next;
        Guard(() => Engine.SetSceneOrigin(x, y, z));
        if (lastListener is { } l)
        {
            SetListener(l.X, l.Y, l.Z, l.ViewX, l.ViewY, l.ViewZ);
        }

        SteamAudioSound[] sounds;
        lock (gate)
        {
            sounds = [.. byVoice.Values];
        }

        foreach (SteamAudioSound sound in sounds)
        {
            sound.Reposition();
        }
    }

    /// <summary>Listener at the eye position (moved back by <see cref="ListenerBackwardOffset"/>), facing along the (unflattened) view vector.</summary>
    public void SetListener(float x, float y, float z, float viewX, float viewY, float viewZ)
    {
        lastListener = (x, y, z, viewX, viewY, viewZ);
        basis.Update(viewX, viewY, viewZ);
        float back = ListenerBackwardOffset;
        SceneOrigin o = origin;
        float lx = (float)(x - (basis.HeadingX * back) - o.X);
        float ly = (float)((double)y - o.Y);
        float lz = (float)(z - (basis.HeadingZ * back) - o.Z);
        Guard(() => Engine.SetListener(lx, ly, lz, basis.ForwardX, basis.ForwardY, basis.ForwardZ, basis.UpX, basis.UpY, basis.UpZ));
    }

    /// <summary>Applies the game's volume sliders and HRTF setting (cheap when nothing changed).</summary>
    public void ApplyLevels(AudioLevels levels)
    {
        ArgumentNullException.ThrowIfNull(levels);
        AudioLevels? previous = lastLevels;
        if (previous is not null && previous.Master == levels.Master && previous.Headphones == levels.Headphones
            && levels.Levels.All(l => previous.Levels.TryGetValue(l.Key, out int old) && old == l.Value))
        {
            return;
        }

        lastLevels = levels;
        Guard(() =>
        {
            Engine.SetMasterGain(Math.Clamp(levels.Master, 0, 100) / 100f);
            foreach ((AudioBus bus, int level) in levels.Levels)
            {
                float trim = trimDb.TryGetValue(bus, out float db) ? MathF.Pow(10f, db / 20f) : 1f;
                Engine.SetBusGain(bus, Math.Clamp(level, 0, 100) / 100f * trim);
            }

            Engine.SetRenderMode(levels.Headphones ? RenderMode.Headphones : RenderMode.Speakers);
        });
    }

    /// <summary>The asset a voice plays (for debug views), or null if it is not one of the session's.</summary>
    public string? DescribeVoice(ulong voice)
    {
        lock (gate)
        {
            return byVoice.TryGetValue(voice, out SteamAudioSound? sound) ? sound.Params.Location?.ToShortString() : null;
        }
    }

    /// <summary>Stops every sound and releases the session's voices and assets.</summary>
    public void Dispose()
    {
        SteamAudioSound[] sounds;
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            sounds = [.. byVoice.Values, .. pending];
        }

        foreach (SteamAudioSound sound in sounds)
        {
            sound.Dispose();
        }

        Assets.Dispose();
    }

    internal void Activated(SteamAudioSound sound, ulong voice)
    {
        lock (gate)
        {
            byVoice[voice] = sound;
        }
    }

    internal void Unregister(SteamAudioSound sound, ulong voice)
    {
        lock (gate)
        {
            if (voice != 0)
            {
                byVoice.Remove(voice);
            }

            pending.Remove(sound);
        }
    }

    /// <summary>Runs an engine call; failures are logged (rate-limited), never thrown into game code.</summary>
    internal void Guard(Action action) => Guard(() =>
    {
        action();
        return 0;
    }, 0);

    internal T Guard<T>(Func<T> action, T fallback)
    {
        try
        {
            return action();
        }
        catch (ObjectDisposedException)
        {
            return fallback;  // the engine is gone (hand-back); the sound is inert
        }
        catch (NativeException ex)
        {
            ReportFailure("engine call failed", ex);
            return fallback;
        }
    }

    internal void ReportFailure(string what, Exception ex)
    {
        long now = Environment.TickCount64;
        long last = Interlocked.Read(ref lastFailureLog);
        if (now - last < 1000 || Interlocked.CompareExchange(ref lastFailureLog, now, last) != last)
        {
            Interlocked.Increment(ref suppressedFailures);
            return;
        }

        int suppressed = Interlocked.Exchange(ref suppressedFailures, 0);
        logger?.Warning(
            "{0}: {1}{2}",
            what,
            ex.Message,
            suppressed > 0 ? string.Create(CultureInfo.InvariantCulture, $" ({suppressed} similar failures suppressed)") : string.Empty);
    }
}

/// <summary>A block position that engine positions are relative to.</summary>
public sealed record SceneOrigin(int X, int Y, int Z)
{
    public static readonly SceneOrigin Zero = new(0, 0, 0);
}

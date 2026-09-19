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
                        logger?.Notification("[vssteamaudio] output device: {0}", e.Type);
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

    /// <summary>Listener at the eye position, facing along the (unflattened) view vector.</summary>
    public void SetListener(float x, float y, float z, float viewX, float viewY, float viewZ)
    {
        basis.Update(viewX, viewY, viewZ);
        Guard(() => Engine.SetListener(x, y, z, basis.ForwardX, basis.ForwardY, basis.ForwardZ, basis.UpX, basis.UpY, basis.UpZ));
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
            "[vssteamaudio] {0}: {1}{2}",
            what,
            ex.Message,
            suppressed > 0 ? string.Create(CultureInfo.InvariantCulture, $" ({suppressed} similar failures suppressed)") : string.Empty);
    }
}

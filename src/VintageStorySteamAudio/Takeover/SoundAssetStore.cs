using System.Runtime.InteropServices;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Takeover;

/// <summary>
/// Native assets for the session, by asset location. Decoded once and shared by every sound that
/// plays them; long Ogg files (music) are streamed. Thread-safe: the game decodes on its thread pool.
/// </summary>
public sealed class SoundAssetStore(AudioEngine engine) : IDisposable
{
    private readonly Dictionary<string, AudioAsset> assets = new(StringComparer.Ordinal);
    private readonly Lock gate = new();

    public int Count
    {
        get
        {
            lock (gate)
            {
                return assets.Count;
            }
        }
    }

    public long MemoryBytes
    {
        get
        {
            lock (gate)
            {
                return assets.Values.Sum(a => a.Info.MemoryBytes);
            }
        }
    }

    public bool TryGet(string location, out AudioAsset asset)
    {
        lock (gate)
        {
            return assets.TryGetValue(location, out asset!);
        }
    }

    /// <summary>Decodes an encoded file (Ogg or WAV), or returns the asset already decoded for this location.</summary>
    public AudioAsset GetOrDecode(string location, ReadOnlySpan<byte> encoded)
    {
        if (TryGet(location, out AudioAsset existing))
        {
            return existing;
        }

        return Add(location, engine.CreateAsset(encoded, location));
    }

    /// <summary>Wraps PCM the game already decoded itself (sounds loaded before the takeover).</summary>
    public AudioAsset GetOrWrapPcm(string location, byte[] pcm, int channels, int sampleRate, int bitsPerSample)
    {
        if (TryGet(location, out AudioAsset existing))
        {
            return existing;
        }

        if (bitsPerSample != 16)
        {
            throw new NotSupportedException($"{location}: {bitsPerSample}-bit PCM is not supported");
        }

        ReadOnlySpan<short> samples = MemoryMarshal.Cast<byte, short>(pcm.AsSpan(0, pcm.Length & ~1));
        return Add(location, engine.CreatePcmAsset(samples, channels, sampleRate, location));
    }

    public void Dispose()
    {
        lock (gate)
        {
            foreach (AudioAsset asset in assets.Values)
            {
                asset.Dispose();
            }

            assets.Clear();
        }
    }

    private AudioAsset Add(string location, AudioAsset created)
    {
        lock (gate)
        {
            // Two threads may decode the same file at once: keep the first, drop the other.
            if (assets.TryGetValue(location, out AudioAsset? winner))
            {
                created.Dispose();
                return winner;
            }

            assets[location] = created;
            return created;
        }
    }
}

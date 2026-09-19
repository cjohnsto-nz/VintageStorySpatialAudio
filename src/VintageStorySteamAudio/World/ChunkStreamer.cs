namespace VintageStorySteamAudio.World;

/// <summary>A chunk position (block coordinates / 32).</summary>
public readonly record struct ChunkKey(int X, int Y, int Z);

/// <summary>
/// Decides which chunks the acoustic scene holds, at which detail, and in what order they are
/// (re)sent: full detail within <see cref="FullRadius"/> chunks of the listener's chunk
/// (horizontally), 2³ super-voxels out to <see cref="LodRadius"/>, and
/// <see cref="VerticalRadius"/> chunks up and down. Nearest first; chunks the game reports dirty
/// are re-read after a short debounce (the game marks chunks dirty in bursts, and for lighting too).
/// Pure bookkeeping: the caller reads and sends chunks.
/// </summary>
public sealed class ChunkStreamer
{
    private readonly Dictionary<ChunkKey, Sent> sent = [];
    private readonly Dictionary<ChunkKey, long> dirty = [];
    private readonly Dictionary<ChunkKey, int> desired = [];
    private List<ChunkKey> order = [];
    private ChunkKey? centre;

    public ChunkStreamer(int fullRadius, int lodRadius, int verticalRadius, int heightInChunks)
    {
        FullRadius = Math.Max(0, fullRadius);
        LodRadius = Math.Max(FullRadius, lodRadius);
        VerticalRadius = Math.Max(0, verticalRadius);
        HeightInChunks = Math.Max(1, heightInChunks);
    }

    public int FullRadius { get; }

    public int LodRadius { get; }

    public int VerticalRadius { get; }

    public int HeightInChunks { get; }

    /// <summary>Milliseconds a dirty chunk waits before it is read again.</summary>
    public long DebounceMs { get; init; } = 250;

    public ChunkKey? Centre => centre;

    public int DesiredCount => desired.Count;

    public int SentCount => sent.Count;

    public int DirtyCount => dirty.Count;

    /// <summary>The detail a chunk should have (0 or 1), or null if it is outside the scene.</summary>
    public int? DesiredLod(ChunkKey key) => desired.TryGetValue(key, out int lod) ? lod : null;

    /// <summary>What was last sent for a chunk: its detail and contents fingerprint.</summary>
    public bool TryGetSent(ChunkKey key, out int lod, out ulong fingerprint)
    {
        if (sent.TryGetValue(key, out Sent s))
        {
            lod = s.Lod;
            fingerprint = s.Fingerprint;
            return true;
        }

        lod = 0;
        fingerprint = 0;
        return false;
    }

    public IEnumerable<ChunkKey> SentKeys => sent.Keys;

    /// <summary>Moves the scene's centre to the listener's chunk (cheap when it has not changed).</summary>
    public void SetCentre(ChunkKey chunk)
    {
        if (centre == chunk)
        {
            return;
        }

        centre = chunk;
        desired.Clear();
        for (int dy = -VerticalRadius; dy <= VerticalRadius; dy++)
        {
            int y = chunk.Y + dy;
            if (y < 0 || y >= HeightInChunks)
            {
                continue;
            }

            for (int dz = -LodRadius; dz <= LodRadius; dz++)
            {
                for (int dx = -LodRadius; dx <= LodRadius; dx++)
                {
                    int ring = Math.Max(Math.Abs(dx), Math.Abs(dz));
                    desired[new ChunkKey(chunk.X + dx, y, chunk.Z + dz)] = ring <= FullRadius ? 0 : 1;
                }
            }
        }

        order = [.. desired.Keys.OrderBy(k => Distance(k, chunk))];
    }

    public void MarkDirty(ChunkKey key, long nowMs)
    {
        if (desired.ContainsKey(key) && !dirty.ContainsKey(key))
        {
            dirty[key] = nowMs;
        }
    }

    /// <summary>
    /// The next chunks to read, nearest first: desired chunks that are loaded and not sent yet (or
    /// sent at another detail), then dirty ones whose debounce has passed.
    /// </summary>
    public IEnumerable<(ChunkKey Key, int Lod)> Next(Func<ChunkKey, bool> isLoaded, long nowMs)
    {
        ArgumentNullException.ThrowIfNull(isLoaded);
        foreach (ChunkKey key in order)
        {
            int lod = desired[key];
            bool stale = !sent.TryGetValue(key, out Sent s) || s.Lod != lod;
            bool due = dirty.TryGetValue(key, out long since) && nowMs - since >= DebounceMs;
            if ((stale || due) && isLoaded(key))
            {
                yield return (key, lod);
            }
        }
    }

    /// <summary>Chunks sent earlier that are no longer wanted or no longer loaded.</summary>
    public List<ChunkKey> Obsolete(Func<ChunkKey, bool> isLoaded)
    {
        ArgumentNullException.ThrowIfNull(isLoaded);
        return [.. sent.Keys.Where(k => !desired.ContainsKey(k) || !isLoaded(k))];
    }

    /// <summary>Records a read; returns true if the contents changed and must be sent.</summary>
    public bool Read(ChunkKey key, int lod, ulong fingerprint)
    {
        dirty.Remove(key);
        if (sent.TryGetValue(key, out Sent s) && s.Lod == lod && s.Fingerprint == fingerprint)
        {
            return false;
        }

        sent[key] = new Sent(lod, fingerprint);
        return true;
    }

    public void Removed(ChunkKey key)
    {
        sent.Remove(key);
        dirty.Remove(key);
    }

    public void Reset()
    {
        sent.Clear();
        dirty.Clear();
    }

    private static int Distance(ChunkKey a, ChunkKey b)
    {
        int dx = a.X - b.X;
        int dy = a.Y - b.Y;
        int dz = a.Z - b.Z;
        return (dx * dx) + (dy * dy) + (dz * dz);
    }

    private readonly record struct Sent(int Lod, ulong Fingerprint);
}

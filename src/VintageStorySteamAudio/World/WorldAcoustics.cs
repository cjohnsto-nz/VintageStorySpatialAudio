using System.Collections.Concurrent;
using System.Diagnostics;
using Newtonsoft.Json;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.World;

/// <summary>What <see cref="WorldAcoustics"/> is doing, for the HUD and .steamaudio scene.</summary>
public sealed record WorldStatus(
    bool Running,
    ChunkKey? Centre,
    int Desired,
    int Sent,
    int Dirty,
    long ChunksRead,
    long ChunksSent,
    double LastTickMs,
    double MaxReadMs,
    (int X, int Y, int Z) Origin,
    string MaterialSource,
    int MaterialCount);

/// <summary>
/// Keeps the engine's world scene in step with the world around the listener (ADR 0007): builds
/// the material and block tables when the level is ready, then each tick reads chunks nearest
/// first within a time budget (full detail close by, the coarse ring beyond), re-reads chunks the
/// game marks dirty, drops chunks that left the radius or unloaded (the client raises no unload
/// event, so sent chunks are polled), and moves the origin when the listener strays far.
/// Main thread, except the ChunkDirty handler (the game may raise it from its tesselation thread).
/// </summary>
internal sealed class WorldAcoustics : IDisposable
{
    public const string MaterialsAsset = "config/acousticmaterials.json";
    public const string MaterialsOverrideFile = "vssteamaudio-materials.json";

    /// <summary>The listener may wander this far (blocks) from the origin before it moves.</summary>
    private const int OriginSlack = 1024;
    private const long UnloadPollMs = 1000;
    private const int TickMs = 50;

    private readonly ICoreClientAPI capi;
    private readonly AudioEngine engine;
    private readonly ILogger logger;
    private readonly Action<int, int, int> setOrigin;
    private readonly ChunkReader reader;
    private readonly ChunkSnapshot snapshot = new();
    private readonly ConcurrentQueue<ChunkKey> dirtyQueue = new();
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly double budgetMs;
    private readonly ChunkStreamer streamer;
    private BlockTable? blocks;
    private long tickListener = -1;
    private long nextUnloadPoll;
    private long chunksRead;
    private long chunksSent;
    private double lastTickMs;
    private double maxReadMs;
    private (int X, int Y, int Z) origin;
    private bool originSet;
    private string materialSource = "none";
    private bool disposed;

    public WorldAcoustics(
        ICoreClientAPI capi, AudioEngine engine, ILogger logger, Action<int, int, int> setOrigin,
        int fullRadius, int lodRadius, int verticalRadius, double budgetMs)
    {
        this.capi = capi;
        this.engine = engine;
        this.logger = logger;
        this.setOrigin = setOrigin;
        this.budgetMs = Math.Clamp(budgetMs, 0.25, 20);
        reader = new ChunkReader(capi);
        streamer = new ChunkStreamer(fullRadius, lodRadius, verticalRadius, Math.Max(1, capi.World.BlockAccessor.MapSizeY / VsaNative.ChunkSize));
    }

    public MaterialTable? Materials => blocks?.Materials;

    /// <summary>Starts streaming (call once the level is finalized: the block list comes from the server).</summary>
    public void Start()
    {
        if (tickListener >= 0 || disposed)
        {
            return;
        }

        LoadMaterials();
        capi.Event.ChunkDirty += OnChunkDirty;
        capi.Event.BlockChanged += OnBlockChanged;
        tickListener = capi.Event.RegisterGameTickListener(_ => Tick(), TickMs);
    }

    /// <summary>Re-reads the materials (the ModConfig override, else the mod's asset) and rebuilds the whole scene.</summary>
    public string Reload()
    {
        LoadMaterials();
        streamer.Reset();
        engine.ClearScene();
        return $"Materials reloaded from {materialSource} ({blocks?.Materials.Count ?? 0} materials); rebuilding the scene.";
    }

    public WorldStatus Status() => new(
        tickListener >= 0, streamer.Centre, streamer.DesiredCount, streamer.SentCount, streamer.DirtyCount,
        chunksRead, chunksSent, lastTickMs, maxReadMs, origin, materialSource, blocks?.Materials.Count ?? 0);

    /// <summary>
    /// What the game has at a block position, for the ray probe: the solid and fluid blocks (code,
    /// class, block material, block entity), a multi-block filler's control block, and how the
    /// scene classifies it.
    /// </summary>
    public string DescribeCell(BlockPos pos)
    {
        IBlockAccessor ba = capi.World.BlockAccessor;
        Block solid = ba.GetBlock(pos, BlockLayersAccess.Solid);
        Block fluid = ba.GetBlock(pos, BlockLayersAccess.Fluid);
        var text = new System.Text.StringBuilder();
        text.Append(solid.Code).Append(" [").Append(solid.GetType().Name).Append(", ").Append(solid.BlockMaterial);
        if (!string.IsNullOrEmpty(solid.EntityClass))
        {
            text.Append(", entity ").Append(solid.EntityClass);
        }

        text.Append(']');
        if (fluid.BlockId != 0)
        {
            text.Append(" + fluid ").Append(fluid.Code);
        }

        if (solid is IMultiblockOffset filler)
        {
            BlockPos control = filler.GetControlBlockPos(pos);
            text.Append(" -> part of ").Append(ba.GetBlock(control).Code).Append(" at ").Append(control.X).Append(',').Append(control.Y).Append(',').Append(control.Z);
        }

        return text.ToString();
    }

    /// <summary>The acoustic material of the block at a position (as the scene sees it), for the HUD.</summary>
    public string DescribeBlock(BlockPos pos)
    {
        if (blocks is null)
        {
            return "-";
        }

        Block block = capi.World.BlockAccessor.GetBlock(pos, BlockLayersAccess.Solid);
        BlockAcoustics a = blocks[block.BlockId];
        if (a.Shape == CellShape.Air)
        {
            Block fluid = capi.World.BlockAccessor.GetBlock(pos, BlockLayersAccess.Fluid);
            if (fluid.BlockId != 0)
            {
                block = fluid;
                a = blocks[fluid.BlockId];
            }
        }

        string shape = a.Shape switch
        {
            CellShape.Air => "not in scene",
            CellShape.Full => "full",
            CellShape.Partial => $"{a.Boxes?.Length ?? 0} box(es)",
            _ => "dynamic boxes",
        };
        return $"{block.Code} ({block.BlockMaterial}) -> {blocks.Materials.NameOf(a.Material)}, {shape}";
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        if (tickListener >= 0)
        {
            capi.Event.UnregisterGameTickListener(tickListener);
            tickListener = -1;
            capi.Event.ChunkDirty -= OnChunkDirty;
            capi.Event.BlockChanged -= OnBlockChanged;
        }

        try
        {
            engine.ClearScene();
        }
        catch (Exception ex) when (ex is NativeException or ObjectDisposedException)
        {
            // The engine is going away too.
        }
    }

    private void LoadMaterials()
    {
        AcousticMaterialConfig? config = null;
        string overridePath = Path.Combine(capi.GetOrCreateDataPath("ModConfig"), MaterialsOverrideFile);
        if (File.Exists(overridePath))
        {
            try
            {
                config = JsonConvert.DeserializeObject<AcousticMaterialConfig>(File.ReadAllText(overridePath));
                materialSource = $"ModConfig/{MaterialsOverrideFile}";
            }
            catch (Exception ex) when (ex is JsonException or IOException or UnauthorizedAccessException)
            {
                logger.Error("{0} is not valid ({1}); using the mod's materials", MaterialsOverrideFile, ex.Message);
            }
        }

        if (config is null)
        {
            config = capi.Assets.TryGet(new AssetLocation("vssteamaudio", MaterialsAsset))?.ToObject<AcousticMaterialConfig>();
            materialSource = config is null ? "built-in defaults" : $"vssteamaudio:{MaterialsAsset}";
        }

        MaterialTable table = MaterialTable.Build(config ?? new AcousticMaterialConfig(), w => logger.Warning("acoustic materials: {0}", w));
        engine.SetSceneMaterials(table.Materials);
        blocks = BlockTable.Build(capi.World, table);
    }

    private void OnChunkDirty(Vec3i chunk, IWorldChunk data, EnumChunkDirtyReason reason) =>
        dirtyQueue.Enqueue(new ChunkKey(chunk.X, chunk.Y, chunk.Z));

    private void OnBlockChanged(BlockPos pos, Block oldBlock)
    {
        MarkBlockDirty(pos.X, pos.InternalY, pos.Z);

        // A door or gate: its other blocks (up to 2 x 4, possibly in the next chunk) changed shape too.
        if (capi.World.BlockAccessor.GetBlockEntity(pos) is not null)
        {
            for (int dy = -3; dy <= 3; dy += 3)
            {
                for (int dz = -2; dz <= 2; dz += 2)
                {
                    for (int dx = -2; dx <= 2; dx += 2)
                    {
                        MarkBlockDirty(pos.X + dx, pos.InternalY + dy, pos.Z + dz);
                    }
                }
            }
        }
    }

    private void MarkBlockDirty(int x, int y, int z) =>
        dirtyQueue.Enqueue(new ChunkKey(x >> 5, y >> 5, z >> 5));

    private void Tick()
    {
        if (blocks is null || capi.World.Player?.Entity is not { } player)
        {
            return;
        }

        long start = clock.ElapsedTicks;
        long now = clock.ElapsedMilliseconds;
        try
        {
            var p = player.Pos;
            int bx = (int)Math.Floor(p.X);
            int by = (int)Math.Floor(p.InternalY);
            int bz = (int)Math.Floor(p.Z);
            UpdateOrigin(bx, by, bz);
            streamer.SetCentre(new ChunkKey(bx >> 5, by >> 5, bz >> 5));
            while (dirtyQueue.TryDequeue(out ChunkKey key))
            {
                streamer.MarkDirty(key, now);
            }

            if (now >= nextUnloadPoll)
            {
                nextUnloadPoll = now + UnloadPollMs;
                foreach (ChunkKey key in streamer.Obsolete(reader.IsLoaded))
                {
                    engine.RemoveSceneChunk(key.X, key.Y, key.Z);
                    streamer.Removed(key);
                }
            }

            foreach ((ChunkKey key, int lod) in streamer.Next(reader.IsLoaded, now))
            {
                long readStart = clock.ElapsedTicks;
                if (reader.Read(key, lod, blocks, snapshot))
                {
                    chunksRead++;
                    if (streamer.Read(key, lod, snapshot.Fingerprint()))
                    {
                        engine.SetSceneChunk(snapshot);
                        chunksSent++;
                    }
                }

                maxReadMs = Math.Max(maxReadMs, Ms(clock.ElapsedTicks - readStart));
                if (Ms(clock.ElapsedTicks - start) >= budgetMs)
                {
                    break;
                }
            }
        }
        catch (NativeException ex)
        {
            logger.Error("world scene update failed: {0}", ex.Message);
        }

        lastTickMs = Ms(clock.ElapsedTicks - start);
    }

    private void UpdateOrigin(int x, int y, int z)
    {
        if (originSet && Math.Abs(x - origin.X) <= OriginSlack && Math.Abs(z - origin.Z) <= OriginSlack
            && Math.Abs(y - origin.Y) <= OriginSlack)
        {
            return;
        }

        // Chunk-aligned, so chunk instances sit on whole-number offsets.
        origin = ((x >> 5) << 5, (y >> 5) << 5, (z >> 5) << 5);
        originSet = true;
        setOrigin(origin.X, origin.Y, origin.Z);
    }

    private static double Ms(long ticks) => ticks * 1000.0 / Stopwatch.Frequency;
}

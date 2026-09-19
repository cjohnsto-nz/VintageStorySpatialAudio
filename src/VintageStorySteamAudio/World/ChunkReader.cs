using Vintagestory.API.Client;
using Vintagestory.API.Common;
using Vintagestory.API.MathTools;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.World;

/// <summary>Every block type's <see cref="BlockAcoustics"/>, indexed by block id. Built on the main thread.</summary>
internal sealed class BlockTable
{
    private readonly BlockAcoustics[] byId;

    private BlockTable(BlockAcoustics[] byId, MaterialTable materials)
    {
        this.byId = byId;
        Materials = materials;
    }

    /// <summary>The material table the classification used.</summary>
    public MaterialTable Materials { get; }

    public BlockAcoustics this[int id] => (uint)id < (uint)byId.Length ? byId[id] : BlockAcoustics.Air;

    public int Count => byId.Length;

    public static BlockTable Build(IClientWorldAccessor world, MaterialTable materials)
    {
        IList<Block> blocks = world.Blocks;
        var byId = new BlockAcoustics[blocks.Count];
        for (int id = 0; id < blocks.Count; id++)
        {
            Block? block = blocks[id];
            if (block is null || block.IsMissing || block.Code is null)
            {
                byId[id] = BlockAcoustics.Air;
                continue;
            }

            byId[id] = BlockClassifier.Classify(Describe(block, block.CollisionBoxes), materials);
        }

        return new BlockTable(byId, materials);
    }

    public static BlockInfo Describe(Block block, Cuboidf[]? boxes) => new(
        block.Code?.ToString() ?? string.Empty,
        block.BlockMaterial.ToString(),
        boxes?.Where(b => b is not null).Select(b => new VsaBox(b.X1, b.Y1, b.Z1, b.X2, b.Y2, b.Z2)).ToArray(),
        !string.IsNullOrEmpty(block.EntityClass));
}

/// <summary>
/// Copies a loaded chunk into a <see cref="ChunkSnapshot"/>: the solid layer (falling back to the
/// fluid layer where the solid block is not in the scene, e.g. reeds in water) through the block
/// table, and the collision boxes of blocks whose shape depends on a block entity (read after the
/// chunk lock is released, since they read block entities). Main thread.
/// </summary>
internal sealed class ChunkReader(ICoreClientAPI capi)
{
    private readonly List<(int Cell, int BlockId)> dynamicCells = [];
    private readonly BlockPos pos = new(0);

    /// <summary>Whether the chunk is loaded and complete on the client.</summary>
    public bool IsLoaded(ChunkKey key)
    {
        IWorldChunk? chunk = capi.World.BlockAccessor.GetChunk(key.X, key.Y, key.Z);
        return chunk is not null && !chunk.Disposed && chunk is not IClientChunk { LoadedFromServer: false };
    }

    /// <summary>Reads a chunk; false if it is not (or no longer) loaded.</summary>
    public bool Read(ChunkKey key, int lod, BlockTable table, ChunkSnapshot snapshot)
    {
        IWorldChunk? chunk = capi.World.BlockAccessor.GetChunk(key.X, key.Y, key.Z);
        if (chunk is null || chunk.Disposed || chunk is IClientChunk { LoadedFromServer: false })
        {
            return false;
        }

        snapshot.Clear();
        snapshot.X = key.X;
        snapshot.Y = key.Y;
        snapshot.Z = key.Z;
        snapshot.Lod = lod;
        if (chunk.Empty)
        {
            return true;
        }

        chunk.Unpack_ReadOnly();
        IChunkBlocks? data = chunk.Data;
        if (data is null)
        {
            return false;
        }

        ushort[] materials = snapshot.Materials;
        dynamicCells.Clear();
        data.TakeBulkReadLock();
        try
        {
            for (int cell = 0; cell < VsaNative.ChunkCells; cell++)
            {
                int id = data.GetBlockIdUnsafe(cell);
                BlockAcoustics block = id != 0 ? table[id] : BlockAcoustics.Air;
                if (block.Shape == CellShape.Air)
                {
                    int fluid = data.GetFluid(cell);
                    if (fluid != 0)
                    {
                        id = fluid;
                        block = table[fluid];
                    }
                }

                switch (block.Shape)
                {
                    case CellShape.Full:
                        materials[cell] = block.Material;
                        break;
                    case CellShape.Partial:
                        snapshot.AddPartial(cell, block.Material, block.Boxes!);
                        break;
                    case CellShape.Dynamic:
                        dynamicCells.Add((cell, id));
                        break;
                    default:
                        break;
                }
            }
        }
        finally
        {
            data.ReleaseBulkReadLock();
        }

        // Doors and the like: their current collision boxes, through the block entity.
        foreach ((int cell, int blockId) in dynamicCells)
        {
            Block block = capi.World.Blocks[blockId];
            BlockAcoustics type = table[blockId];
            pos.Set((key.X * VsaNative.ChunkSize) + (cell % 32), (key.Y * VsaNative.ChunkSize) + (cell / 1024), (key.Z * VsaNative.ChunkSize) + ((cell / 32) % 32));
            Cuboidf[]? boxes = block.GetCollisionBoxes(capi.World.BlockAccessor, pos);
            BlockAcoustics current = boxes is null
                ? BlockAcoustics.Air
                : BlockClassifier.Classify(BlockTable.Describe(block, boxes) with { HasBlockEntity = false }, table.Materials);
            if (current.Shape == CellShape.Full)
            {
                materials[cell] = type.Material;
            }
            else if (current.Shape == CellShape.Partial)
            {
                snapshot.AddPartial(cell, type.Material, current.Boxes!);
            }
        }

        return true;
    }
}

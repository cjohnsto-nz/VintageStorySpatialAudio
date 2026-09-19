using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.World;

/// <summary>How a block occupies its cell in the acoustic scene.</summary>
public enum CellShape : byte
{
    /// <summary>Not in the scene (air, plants, fire, blocks without collision).</summary>
    Air,

    /// <summary>The whole cell (full cubes, leaves, liquids).</summary>
    Full,

    /// <summary>Its collision boxes, the same for every instance (slabs, stairs, fences).</summary>
    Partial,

    /// <summary>Collision boxes that depend on a block entity (doors, trapdoors): read per snapshot.</summary>
    Dynamic,
}

/// <summary>What the classifier needs to know about a block type.</summary>
/// <param name="Code">"domain:path".</param>
/// <param name="BlockMaterial">The EnumBlockMaterial name.</param>
/// <param name="CollisionBoxes">Its collision boxes in block units, or null.</param>
/// <param name="HasBlockEntity">Whether it has a block entity (its collision may change without the block changing).</param>
public readonly record struct BlockInfo(string Code, string BlockMaterial, IReadOnlyList<VsaBox>? CollisionBoxes, bool HasBlockEntity);

/// <summary>A block type's acoustic classification.</summary>
public readonly record struct BlockAcoustics(ushort Material, CellShape Shape, VsaBox[]? Boxes)
{
    public static readonly BlockAcoustics Air = new(0, CellShape.Air, null);
}

/// <summary>
/// Classifies block types for the scene. The material comes from <see cref="MaterialTable"/>;
/// the shape from its kind and collision boxes: porous and liquid materials fill the cell
/// (leaves have no collision boxes at all), solids are full cubes or their boxes, and a solid
/// without boxes (a torch, a sign) is left out.
/// </summary>
public static class BlockClassifier
{
    public static BlockAcoustics Classify(BlockInfo block, MaterialTable materials)
    {
        ArgumentNullException.ThrowIfNull(materials);
        ushort material = materials.Resolve(block.Code, block.BlockMaterial);
        switch (materials.KindOf(material))
        {
            case MaterialKind.Air:
                return BlockAcoustics.Air;
            case MaterialKind.Liquid:
            case MaterialKind.Porous:
                return new BlockAcoustics(material, CellShape.Full, null);
            default:
                break;
        }

        IReadOnlyList<VsaBox>? boxes = block.CollisionBoxes;
        if (boxes is null || boxes.Count == 0)
        {
            return BlockAcoustics.Air;
        }

        if (boxes.Count == 1 && IsUnitCube(boxes[0]))
        {
            return new BlockAcoustics(material, CellShape.Full, null);
        }

        VsaBox[] clamped = boxes.Select(Clamp).Where(b => b.MaxX > b.MinX && b.MaxY > b.MinY && b.MaxZ > b.MinZ).ToArray();
        if (clamped.Length == 0)
        {
            return BlockAcoustics.Air;
        }

        return new BlockAcoustics(material, block.HasBlockEntity ? CellShape.Dynamic : CellShape.Partial, clamped);
    }

    public static bool IsUnitCube(VsaBox b) =>
        b.MinX <= 0f && b.MinY <= 0f && b.MinZ <= 0f && b.MaxX >= 1f && b.MaxY >= 1f && b.MaxZ >= 1f;

    /// <summary>Boxes may reach outside the block (fences stand 1.5 high); the scene keeps each block to its cell.</summary>
    public static VsaBox Clamp(VsaBox b) => new(
        Math.Clamp(b.MinX, 0f, 1f), Math.Clamp(b.MinY, 0f, 1f), Math.Clamp(b.MinZ, 0f, 1f),
        Math.Clamp(b.MaxX, 0f, 1f), Math.Clamp(b.MaxY, 0f, 1f), Math.Clamp(b.MaxZ, 0f, 1f));
}

using Vintagestory.API.Common;

namespace SpatialAudioUnixNatives;

/// <summary>
/// Does nothing. The pack is a code mod so that the game unpacks it to disk, where the Spatial
/// Audio mod finds the libraries beside this assembly.
/// </summary>
public sealed class NativePackModSystem : ModSystem
{
    public override bool ShouldLoad(EnumAppSide forSide) => forSide == EnumAppSide.Client;
}

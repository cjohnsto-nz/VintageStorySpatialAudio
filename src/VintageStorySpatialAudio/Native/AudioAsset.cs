using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace VintageStorySpatialAudio.Native;

public sealed record AssetInfo(int Channels, int SampleRate, long Frames, AssetStorage Storage, long MemoryBytes, double DurationSeconds);

/// <summary>
/// Decoded (or streamable) audio in native memory. Reference counted natively: disposing drops
/// this wrapper's reference; voices hold their own, so disposing while voices play is safe.
/// Independent of the engine's lifetime.
/// </summary>
public sealed class AudioAsset : IDisposable
{
    private readonly AssetHandle handle;

    internal AudioAsset(nint asset)
    {
        handle = new AssetHandle(asset);
        Info = ReadInfo();
    }

    public AssetInfo Info { get; }

    public void Dispose() => handle.Dispose();

    internal Lease Acquire() => new(handle);

    private AssetInfo ReadInfo()
    {
        using Lease lease = Acquire();
        var info = new VsaAssetInfo { StructSize = (uint)Unsafe.SizeOf<VsaAssetInfo>() };
        NativeException.ThrowIfFailed(VsaNative.AssetGetInfo(lease.Handle, ref info), "vsa_asset_get_info");
        return new AssetInfo(
            (int)info.Channels, (int)info.SampleRate, (long)info.Frames, (AssetStorage)info.Storage, (long)info.MemoryBytes, info.DurationSeconds);
    }

    internal readonly ref struct Lease
    {
        private readonly SafeHandle owner;
        private readonly bool added;

        public Lease(SafeHandle owner)
        {
            this.owner = owner;
            bool ok = false;
            owner.DangerousAddRef(ref ok);
            added = ok;
            Handle = owner.DangerousGetHandle();
        }

        public nint Handle { get; }

        public void Dispose()
        {
            if (added)
            {
                owner.DangerousRelease();
            }
        }
    }

    private sealed class AssetHandle : SafeHandle
    {
        public AssetHandle(nint asset)
            : base(0, ownsHandle: true) => SetHandle(asset);

        public override bool IsInvalid => handle == 0;

        protected override bool ReleaseHandle()
        {
            VsaNative.AssetRelease(handle);
            return true;
        }
    }
}

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Tests;

/// <summary>
/// The managed structs must match vsaudio.h byte for byte. These expectations are the
/// 64-bit layouts of the C structs (the only targets we ship); the native side also
/// rejects any struct_size it does not expect.
/// </summary>
public sealed class NativeLayoutTests
{
    [Fact]
    public void VersionInfo_matches_header()
    {
        Assert.Equal(40, Unsafe.SizeOf<VsaVersionInfo>());
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaVersionInfo>(nameof(VsaVersionInfo.BuildDescription)));
    }

    [Fact]
    public unsafe void EngineConfig_matches_header()
    {
        Assert.Equal(32, sizeof(VsaEngineConfig));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.Log)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.LogUserData)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.RayTracer)));
        Assert.Equal(28, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.Flags)));
    }

    [Fact]
    public void EngineInfo_matches_header() => Assert.Equal(12, Unsafe.SizeOf<VsaEngineInfo>());

    [Fact]
    public void SelfTestReport_matches_header()
    {
        Assert.Equal(24, Unsafe.SizeOf<VsaSelfTestReport>());
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaSelfTestReport>(nameof(VsaSelfTestReport.ElapsedMs)));
    }

    [Theory]
    [InlineData(RayTracer.Auto, 0u)]
    [InlineData(RayTracer.Embree, 1u)]
    [InlineData(RayTracer.Steam, 2u)]
    public void RayTracer_values_match_header(RayTracer value, uint expected) => Assert.Equal(expected, (uint)value);
}

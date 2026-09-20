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
        Assert.Equal(144, sizeof(VsaEngineConfig));
        Assert.Equal(112, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.PathingRange)));
        Assert.Equal(132, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.PathingSources)));
        Assert.Equal(136, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.PathingMaxProbes)));
        Assert.Equal(76, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.DirectRateHz)));
        Assert.Equal(80, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.ReflectionSources)));
        Assert.Equal(92, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.ReflectionDuration)));
        Assert.Equal(108, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.ReflectionTransition)));
        Assert.Equal(64, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.HrtfSofaPath)));
        Assert.Equal(52, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.MaxRealVoices)));
        Assert.Equal(56, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.MaxBinauralVoices)));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.Log)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.LogUserData)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.RayTracer)));
        Assert.Equal(28, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.Flags)));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.SampleRate)));
        Assert.Equal(36, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.BlockFrames)));
        Assert.Equal(40, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.MaxVoices)));
        Assert.Equal(44, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.ResamplerQuality)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaEngineConfig>(nameof(VsaEngineConfig.StreamThresholdMs)));
    }

    [Fact]
    public unsafe void AssetDesc_matches_header()
    {
        Assert.Equal(48, sizeof(VsaAssetDesc));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.Data)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.Size)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.Name)));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.Storage)));
        Assert.Equal(36, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.PcmChannels)));
        Assert.Equal(40, (int)Marshal.OffsetOf<VsaAssetDesc>(nameof(VsaAssetDesc.PcmSampleRate)));
    }

    [Fact]
    public void AssetInfo_matches_header()
    {
        Assert.Equal(40, Unsafe.SizeOf<VsaAssetInfo>());
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaAssetInfo>(nameof(VsaAssetInfo.Frames)));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaAssetInfo>(nameof(VsaAssetInfo.DurationSeconds)));
    }

    [Fact]
    public void VoiceDesc_and_status_match_header()
    {
        Assert.Equal(48, Unsafe.SizeOf<VsaVoiceDesc>());
        Assert.Equal(28, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.Spatial)));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.PositionX)));
        Assert.Equal(44, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.MinDistance)));
        Assert.Equal(52, Unsafe.SizeOf<VsaListener>());
        Assert.Equal(40, (int)Marshal.OffsetOf<VsaListener>(nameof(VsaListener.RenderOffsetX)));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.Asset)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.Gain)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaVoiceDesc>(nameof(VsaVoiceDesc.Looping)));
        Assert.Equal(16, Unsafe.SizeOf<VsaVoiceStatus>());
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaVoiceStatus>(nameof(VsaVoiceStatus.PositionSeconds)));
    }

    [Fact]
    public unsafe void Output_structs_match_header()
    {
        Assert.Equal(512, sizeof(VsaDeviceId));
        Assert.Equal(776, sizeof(VsaDeviceInfo));
        Assert.Equal(264, (int)Marshal.OffsetOf<VsaDeviceInfo>(nameof(VsaDeviceInfo.Id)));
        Assert.Equal(24, sizeof(VsaOutputDesc));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaOutputDesc>(nameof(VsaOutputDesc.DeviceId)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaOutputDesc>(nameof(VsaOutputDesc.Channels)));
    }

    [Fact]
    public unsafe void EngineStats_matches_header()
    {
        Assert.Equal(360, sizeof(VsaEngineStats));
        Assert.Equal(36, (int)Marshal.OffsetOf<VsaEngineStats>(nameof(VsaEngineStats.LimiterPeakReductionDb)));
        Assert.Equal(40, (int)Marshal.OffsetOf<VsaEngineStats>(nameof(VsaEngineStats.RealVoices)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaEngineStats>(nameof(VsaEngineStats.BlocksRendered)));
        Assert.Equal(80, (int)Marshal.OffsetOf<VsaEngineStats>(nameof(VsaEngineStats.RenderTimeAvgUs)));
        Assert.Equal(104, (int)Marshal.OffsetOf<VsaEngineStats>(nameof(VsaEngineStats.DeviceName)));
    }

    [Fact]
    public void Event_matches_header()
    {
        Assert.Equal(32, Unsafe.SizeOf<VsaEvent>());
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaEvent>(nameof(VsaEvent.Voice)));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaEvent>(nameof(VsaEvent.Token)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaEvent>(nameof(VsaEvent.Flags)));
    }

    [Fact]
    public void Enum_values_match_header()
    {
        Assert.Equal(12, (int)VsaResult.Capacity);
        Assert.Equal(3u, (uint)ResamplerQuality.High);
        Assert.Equal(4u, (uint)AudioBus.Music);
        Assert.Equal(VsaNative.BusCount, Enum.GetValues<AudioBus>().Length);
        Assert.Equal(3u, (uint)AssetFormat.PcmS16);
        Assert.Equal(2u, (uint)AssetStorage.Streamed);
        Assert.Equal(2u, (uint)VoiceState.Paused);
        Assert.Equal(1u, (uint)OutputKind.Device);
        Assert.Equal(2u, (uint)OutputKind.Spatial);
        Assert.Equal(6u, (uint)EngineEventType.DeviceRestored);
        Assert.Equal(2u, (uint)SpatialMode.Listener);
        Assert.Equal(1u, (uint)RenderMode.Speakers);
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

    [Fact]
    public unsafe void Scene_structs_match_header()
    {
        Assert.Equal(56, sizeof(VsaAcousticMaterial));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaAcousticMaterial>(nameof(VsaAcousticMaterial.Name)));
        Assert.Equal(24, sizeof(VsaBox));
        Assert.Equal(16, sizeof(VsaPartialBlock));
        Assert.Equal(56, sizeof(VsaChunkDesc));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaChunkDesc>(nameof(VsaChunkDesc.Materials)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaChunkDesc>(nameof(VsaChunkDesc.Boxes)));
        Assert.Equal(88, sizeof(VsaSceneStats));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaSceneStats>(nameof(VsaSceneStats.LastBuildMs)));
        Assert.Equal(72, (int)Marshal.OffsetOf<VsaSceneStats>(nameof(VsaSceneStats.Origin)));
        Assert.Equal(56, sizeof(VsaChunkMesh));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaChunkMesh>(nameof(VsaChunkMesh.Vertices)));
        Assert.Equal(3u, (uint)MaterialKind.Solid);
        Assert.Equal(76, sizeof(VsaRayHit));
        Assert.Equal(48, sizeof(VsaAudibleVoice));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaAudibleVoice>(nameof(VsaAudibleVoice.Voice)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaAudibleVoice>(nameof(VsaAudibleVoice.HeardDb)));
        Assert.Equal(64, sizeof(VsaSourceDebug));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaSourceDebug>("Position"));
        Assert.Equal(60, (int)Marshal.OffsetOf<VsaSourceDebug>(nameof(VsaSourceDebug.Crossings)));
        Assert.Equal(80, Marshal.SizeOf<VsaSimulationStats>());
        Assert.Equal(68, (int)Marshal.OffsetOf<VsaSimulationStats>(nameof(VsaSimulationStats.OriginX)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaSimulationStats>(nameof(VsaSimulationStats.RateHz)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaRayHit>(nameof(VsaRayHit.Triangle)));
        Assert.Equal(60, (int)Marshal.OffsetOf<VsaRayHit>(nameof(VsaRayHit.Cell)));
    }

    [Fact]
    public unsafe void Reflection_structs_match_header()
    {
        Assert.Equal(120, sizeof(VsaReflectionStats));
        Assert.Equal(52, (int)Marshal.OffsetOf<VsaReflectionStats>(nameof(VsaReflectionStats.Reserved)));
        Assert.Equal(56, (int)Marshal.OffsetOf<VsaReflectionStats>(nameof(VsaReflectionStats.Ticks)));
        Assert.Equal(88, (int)Marshal.OffsetOf<VsaReflectionStats>("ListenerReverbTimes"));
        Assert.Equal(100, (int)Marshal.OffsetOf<VsaReflectionStats>(nameof(VsaReflectionStats.OutputDb)));
        Assert.Equal(108, (int)Marshal.OffsetOf<VsaReflectionStats>("Listener"));
        Assert.Equal(56, sizeof(VsaReflectionSource));
        Assert.Equal(8, (int)Marshal.OffsetOf<VsaReflectionSource>(nameof(VsaReflectionSource.Voice)));
        Assert.Equal(52, (int)Marshal.OffsetOf<VsaReflectionSource>(nameof(VsaReflectionSource.Delay)));
        Assert.Equal(40, sizeof(VsaRaySegment));
        Assert.Equal(32, (int)Marshal.OffsetOf<VsaRaySegment>(nameof(VsaRaySegment.Energy)));
        Assert.Equal(4u, VsaNative.EngineFlagNoReflections);
        Assert.Equal(8u, VsaNative.EngineFlagNoPathing);
        Assert.Equal(128, sizeof(VsaPathingStats));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaPathingStats>(nameof(VsaPathingStats.Bakes)));
        Assert.Equal(48, (int)Marshal.OffsetOf<VsaPathingStats>("BoxCentre"));
        Assert.Equal(72, (int)Marshal.OffsetOf<VsaPathingStats>(nameof(VsaPathingStats.Ticks)));
        Assert.Equal(112, (int)Marshal.OffsetOf<VsaPathingStats>("Listener"));
        Assert.Equal(44, (int)Marshal.OffsetOf<VsaPathingStats>(nameof(VsaPathingStats.CancelledBakes)));
        Assert.Equal(124, (int)Marshal.OffsetOf<VsaPathingStats>(nameof(VsaPathingStats.ProbeSpacing)));
        Assert.Equal(32, sizeof(VsaPathSegment));
        Assert.Equal(72, sizeof(VsaThreadStats));
        Assert.Equal(16, (int)Marshal.OffsetOf<VsaThreadStats>(nameof(VsaThreadStats.CpuMs)));
        Assert.Equal(24, (int)Marshal.OffsetOf<VsaThreadStats>("Name"));
        Assert.Equal(20, (int)Marshal.OffsetOf<VsaPathSegment>("To"));
    }
}

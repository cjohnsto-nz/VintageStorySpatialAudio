using Vintagestory.API.Client;

namespace VintageStorySteamAudio.Diagnostics;

/// <summary>
/// Marks the game's frame boundary for <see cref="PerfMonitor"/>, once per frame, before
/// anything is drawn. It is registered whenever the mod loads, not only when the mod has taken
/// the audio over, so a run with <c>TakeOverGameAudio</c> off — vanilla OpenAL playing — is
/// measured by the same instrument as a run with it on. That is the baseline the mod's frame
/// times are compared against (docs/investigations/performance.md).
/// </summary>
internal sealed class FrameProbe : IRenderer
{
    public double RenderOrder => 0.0;

    public int RenderRange => 0;

    public void OnRenderFrame(float deltaTime, EnumRenderStage stage) => PerfMonitor.Instance.Frame();

    public void Dispose()
    {
    }
}

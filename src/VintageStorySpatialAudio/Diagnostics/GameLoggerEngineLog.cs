using Vintagestory.API.Common;
using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Diagnostics;

/// <summary>Routes native engine log output into the mod's game logger.</summary>
internal sealed class GameLoggerEngineLog(ILogger logger) : IEngineLog
{
    public void Write(EngineLogLevel level, string message)
    {
        switch (level)
        {
            case EngineLogLevel.Debug:
                logger.VerboseDebug("[native] {0}", message);
                break;
            case EngineLogLevel.Info:
                logger.Notification("[native] {0}", message);
                break;
            case EngineLogLevel.Warning:
                logger.Warning("[native] {0}", message);
                break;
            default:
                logger.Error("[native] {0}", message);
                break;
        }
    }
}

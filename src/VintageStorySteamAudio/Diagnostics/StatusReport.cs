using System.Globalization;
using System.Text;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio.Diagnostics;

/// <summary>Everything the mod knows about its own health, rendered for logs, chat and the doctor tool.</summary>
public sealed record StatusReport
{
    public EngineVersion? Version { get; init; }

    public EngineInfo? Engine { get; init; }

    public SelfTestResult? SelfTest { get; init; }

    public VerificationReport? Verification { get; init; }

    /// <summary>Why the engine could not start, if it could not.</summary>
    public string? EngineError { get; init; }

    public bool TakeoverPossible => Engine is not null && SelfTest is { Passed: true } && Verification is { AllPassed: true };

    public string Render(bool includePassingTargets = false)
    {
        var text = new StringBuilder();
        if (Version is not null)
        {
            text.AppendLine(CultureInfo.InvariantCulture, $"Native engine {Version.Engine} (ABI {Version.AbiVersion}), Steam Audio {Version.SteamAudio}, {Version.BuildDescription}");
        }

        if (EngineError is not null)
        {
            text.AppendLine(CultureInfo.InvariantCulture, $"Engine: FAILED to start: {EngineError}");
        }
        else if (Engine is not null)
        {
            text.AppendLine(CultureInfo.InvariantCulture, $"Engine: running, ray tracer {Engine.ActiveRayTracer} (Embree available: {(Engine.EmbreeAvailable ? "yes" : "no")})");
        }

        if (SelfTest is not null)
        {
            text.AppendLine(
                CultureInfo.InvariantCulture,
                $"Self-test: {(SelfTest.Passed ? "passed" : "FAILED")} (occlusion through wall {SelfTest.OcclusionThroughWall:0.###}, " +
                $"clear path {SelfTest.OcclusionClearPath:0.###}, {SelfTest.ElapsedMs:0.##} ms)");
        }

        if (Verification is not null)
        {
            text.AppendLine(CultureInfo.InvariantCulture, $"Game integration points: {Verification.PassedCount}/{Verification.Entries.Count} verified");
            IEnumerable<VerificationEntry> shown = includePassingTargets ? Verification.Entries : Verification.Failures;
            foreach (VerificationEntry entry in shown)
            {
                text.Append("  [").Append(entry.Status == VerificationStatus.Ok ? "ok" : entry.Status.ToString()).Append("] ")
                    .Append(entry.Id).Append(": ").AppendLine(entry.Description);
                if (entry.Detail is not null)
                {
                    text.Append("      ").AppendLine(entry.Detail);
                }
            }
        }

        text.Append(TakeoverPossible
            ? "Audio takeover: ready (takeover itself arrives in Phase 2; vanilla audio is still playing)."
            : "Audio takeover: would be REFUSED; vanilla audio stays in charge.");
        return text.ToString();
    }
}

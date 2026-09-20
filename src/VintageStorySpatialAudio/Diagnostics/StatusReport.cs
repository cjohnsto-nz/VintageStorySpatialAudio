using System.Globalization;
using System.Text;
using VintageStorySpatialAudio.Native;
using VintageStorySpatialAudio.Platform;

namespace VintageStorySpatialAudio.Diagnostics;

/// <summary>Everything the mod knows about its own health, rendered for logs, chat and the doctor tool.</summary>
public sealed record StatusReport
{
    public EngineVersion? Version { get; init; }

    public EngineInfo? Engine { get; init; }

    public SelfTestResult? SelfTest { get; init; }

    public VerificationReport? Verification { get; init; }

    /// <summary>Why the engine could not start, if it could not.</summary>
    public string? EngineError { get; init; }

    /// <summary>Whether the game's audio is being played by the engine right now.</summary>
    public bool TakeoverActive { get; init; }

    /// <summary>Why the takeover was not applied when it could have been (disabled, or it failed).</summary>
    public string? TakeoverNote { get; init; }

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

        if (TakeoverActive)
        {
            text.Append("Audio takeover: ACTIVE (the game's sounds play through Steam Audio).");
        }
        else if (TakeoverNote is not null)
        {
            text.Append(CultureInfo.InvariantCulture, $"Audio takeover: not applied ({TakeoverNote}); vanilla audio stays in charge.");
        }
        else
        {
            text.Append(TakeoverPossible
                ? "Audio takeover: ready (applied when a world starts)."
                : "Audio takeover: would be REFUSED; vanilla audio stays in charge.");
        }

        return text.ToString();
    }
}

using System.Globalization;
using Vintagestory.API.Client;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Diagnostics;

/// <summary>
/// An AV receiver style speaker check: a noise burst from each 7.1.4 speaker position in turn
/// (head-locked, so turning does not matter), named in chat as it plays, then one from straight
/// overhead. Played through the engine's current output, so with Windows Spatial Audio the
/// receiver's display shows which speakers light up. Main thread only.
/// </summary>
internal sealed class SpeakerTest(AudioEngine engine) : IDisposable
{
    private const int Rate = 48000;
    private const float BurstSeconds = 1.2f;
    private const int StepMs = 1600;
    private const float Distance = 2f;

    // Name, azimuth (degrees clockwise from ahead), elevation: speaker placements as the engine pans them.
    private static readonly (string Name, float Azimuth, float Elevation)[] Steps =
    [
        ("front left", -30, 0),
        ("centre", 0, 0),
        ("front right", 30, 0),
        ("side right", 90, 0),
        ("back right", 150, 0),
        ("back left", -150, 0),
        ("side left", -90, 0),
        ("top front left", -45, 45),
        ("top front right", 45, 45),
        ("top back right", 135, 45),
        ("top back left", -135, 45),
        ("straight overhead (all four heights)", 0, 90),
    ];

    private AudioAsset? noise;
    private Voice? current;
    private long callback = -1;
    private ICoreClientAPI? api;

    public bool Running => callback >= 0 || current is not null;

    /// <summary>Starts (or restarts) the sequence. The output must already be open.</summary>
    public string Start(ICoreClientAPI clientApi)
    {
        Stop();
        api = clientApi;
        noise ??= engine.CreatePcmAsset(Burst(), 1, Rate, "speaker-test");
        Play(0);
        EngineStats stats = engine.GetStats();
        return string.Create(
            CultureInfo.InvariantCulture,
            $"Speaker test on {SteamAudioModSystem.DescribeOutput(stats)}, {stats.Channels} channels: " +
            $"{Steps.Length} bursts, {StepMs / 1000.0:0.#} s apart. .steamaudio speakertest stop ends it.");
    }

    public void Stop()
    {
        if (callback >= 0)
        {
            api?.Event.UnregisterCallback(callback);
            callback = -1;
        }

        current?.Dispose();
        current = null;
    }

    public void Dispose()
    {
        Stop();
        noise?.Dispose();
        noise = null;
    }

    private void Play(int step)
    {
        callback = -1;
        current?.Dispose();
        current = null;
        if (step >= Steps.Length || api is null || noise is null)
        {
            api?.ShowChatMessage("Speaker test finished.");
            return;
        }

        (string name, float azimuth, float elevation) = Steps[step];
        double az = azimuth * Math.PI / 180.0;
        double el = elevation * Math.PI / 180.0;
        // Listener space: +x right, +y up, -z ahead.
        var placement = new VoicePlacement(
            SpatialMode.Listener,
            (float)(Distance * Math.Sin(az) * Math.Cos(el)),
            (float)(Distance * Math.Sin(el)),
            (float)(-Distance * Math.Cos(az) * Math.Cos(el)),
            MinDistance: Distance);
        current = engine.CreateVoice(noise, AudioBus.Sound, 1f, 1f, looping: false, placement);
        current.Start();
        api.ShowChatMessage(string.Create(CultureInfo.InvariantCulture, $"Speaker test {step + 1}/{Steps.Length}: {name}"));
        callback = api.Event.RegisterCallback(_ => Play(step + 1), StepMs);
    }

    /// <summary>Pink-ish noise (white through a gentle low-pass) with 20 ms fades, at -12 dBFS.</summary>
    private static short[] Burst()
    {
        int frames = (int)(BurstSeconds * Rate);
        int fade = Rate / 50;
        var samples = new short[frames];
        var random = new Random(1234);
        double low = 0;
        for (int i = 0; i < frames; i++)
        {
            double white = (random.NextDouble() * 2.0) - 1.0;
            low += 0.25 * (white - low);
            double x = (0.5 * white) + (0.5 * low);
            double envelope = Math.Min(1.0, Math.Min(i, frames - 1 - i) / (double)fade);
            samples[i] = (short)Math.Round(0.25 * x * envelope * short.MaxValue);
        }

        return samples;
    }
}

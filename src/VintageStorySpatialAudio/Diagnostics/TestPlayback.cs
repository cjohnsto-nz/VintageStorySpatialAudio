using System.Globalization;
using System.Text;
using Vintagestory.API.Client;
using Vintagestory.API.Common;
using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Diagnostics;

/// <summary>
/// Phase 1 listening test: plays game sound assets through our own engine and output device
/// (WASAPI shared mode, alongside the game's OpenAL audio). Main thread only.
/// </summary>
internal sealed class TestPlayback(AudioEngine engine, ILogger logger, string? deviceName) : IDisposable
{
    private readonly List<(Voice Voice, string Name)> playing = [];
    private readonly EngineEvent[] events = new EngineEvent[64];
    private bool deviceOpen;

    public int PlayingCount => playing.Count;

    public string Play(ICoreClientAPI api, string name, float volume, float pitch)
    {
        AssetLocation location = ToSoundLocation(name);
        IAsset? asset = api.Assets.TryGet(location);
        if (asset?.Data is not { Length: > 0 } data)
        {
            return $"Sound asset '{location}' not found.";
        }

        string? deviceError = EnsureDevice();
        if (deviceError is not null)
        {
            return deviceError;
        }

        // The voice holds its own reference to the asset, so ours can go straight away.
        using AudioAsset audio = engine.CreateAsset(data, location.ToString());
        Voice voice = engine.CreateVoice(audio, AudioBus.Sound, volume, pitch);
        voice.Start();
        playing.Add((voice, location.ToString()));

        AssetInfo info = audio.Info;
        return string.Create(
            CultureInfo.InvariantCulture,
            $"Playing {location}: {info.DurationSeconds:0.00} s, {info.Channels} ch, {info.SampleRate} Hz, {info.Storage}, " +
            $"volume {volume:0.##}, pitch {pitch:0.##} on '{engine.GetStats().DeviceName}'.");
    }

    public int StopAll()
    {
        int count = playing.Count;
        foreach ((Voice voice, _) in playing)
        {
            voice.Dispose();
        }

        playing.Clear();
        return count;
    }

    /// <summary>Releases voices that finished and reports device changes. Call periodically.</summary>
    public void Tick()
    {
        int count;
        while ((count = engine.PollEvents(events)) > 0)
        {
            for (int i = 0; i < count; i++)
            {
                EngineEvent e = events[i];
                switch (e.Type)
                {
                    case EngineEventType.VoiceEnded:
                        int index = playing.FindIndex(p => p.Voice.Handle == e.Voice);
                        if (index >= 0)
                        {
                            playing[index].Voice.Dispose();
                            playing.RemoveAt(index);
                        }

                        break;
                    case EngineEventType.DeviceLost:
                        logger.Warning("Output device lost; the engine is trying to reopen it.");
                        break;
                    case EngineEventType.DeviceRestored:
                        logger.Notification("Output device restored.");
                        break;
                    case EngineEventType.DeviceRerouted:
                        logger.Notification("Output followed the new default device.");
                        break;
                    default:
                        break;
                }
            }
        }
    }

    public string ListDevices()
    {
        IReadOnlyList<AudioDevice> devices = engine.EnumerateDevices();
        var text = new StringBuilder();
        text.AppendLine(CultureInfo.InvariantCulture, $"{devices.Count} playback device(s):");
        foreach (AudioDevice device in devices)
        {
            text.Append("  ").Append(device.IsDefault ? "* " : "  ").AppendLine(device.Name);
        }

        text.Append(deviceOpen ? $"In use: '{engine.GetStats().DeviceName}'." : "No device open yet (opened on the first .spatialaudio play).");
        return text.ToString();
    }

    public void Dispose() => StopAll();

    /// <summary>Opens the test output unless one is open (the takeover's, or an earlier test's). Returns an error message or null.</summary>
    internal string? EnsureDevice()
    {
        if (deviceOpen)
        {
            return null;
        }

        // With the takeover the game's output device is already open: play there.
        if (string.IsNullOrWhiteSpace(deviceName) && engine.GetStats().Output != OutputKind.None)
        {
            deviceOpen = true;
            return null;
        }

        AudioDevice? device = null;
        if (!string.IsNullOrWhiteSpace(deviceName))
        {
            device = engine.EnumerateDevices().FirstOrDefault(d => d.Name.Contains(deviceName, StringComparison.OrdinalIgnoreCase));
            if (device is null)
            {
                return $"No playback device matches '{deviceName}' (TestOutputDevice in {Config.SpatialAudioConfig.FileName}). Try .spatialaudio devices.";
            }
        }

        engine.OpenDevice(device);
        deviceOpen = true;
        EngineStats stats = engine.GetStats();
        logger.Notification(
            "Test output opened: '{0}', {1} Hz, {2} channels, period {3} frames",
            stats.DeviceName, stats.SampleRate, stats.Channels, stats.DevicePeriodFrames);
        return null;
    }

    /// <summary>"effect/woodswitch" -> game:sounds/effect/woodswitch.ogg; full locations pass through.</summary>
    internal static AssetLocation ToSoundLocation(string name)
    {
        var location = new AssetLocation(name.Trim());
        string path = location.Path;
        if (!path.StartsWith("sounds/", StringComparison.Ordinal))
        {
            path = "sounds/" + path;
        }

        if (!path.EndsWith(".ogg", StringComparison.Ordinal) && !path.EndsWith(".wav", StringComparison.Ordinal))
        {
            path += ".ogg";
        }

        return new AssetLocation(location.Domain, path);
    }
}

using VintageStorySpatialAudio.Native;

namespace VintageStorySpatialAudio.Tests;

/// <summary>
/// The managed bindings against the real native engine, rendered offline. Skipped when the
/// natives are not built. (Signal-level behaviour is covered by the native test suites; these
/// check that the bindings carry every call and struct across correctly.)
/// </summary>
[Collection(NativeEngineGroup.Name)]
public sealed class NativeEngineTests
{
    private const int Rate = 48000;

    [Fact]
    public void Voices_play_through_the_bindings_and_report_state_immediately()
    {
        using AudioEngine engine = CreateEngine();
        using AudioAsset asset = engine.CreatePcmAsset(Sine(480, Rate, Rate / 10, 0.5), 1, Rate, "tone");
        Assert.Equal(new AssetInfo(1, Rate, Rate / 10, AssetStorage.Decoded, Rate / 10 * 2, 0.1), asset.Info);

        using Voice voice = engine.CreateVoice(asset, AudioBus.Ambient, gain: 0.5f, looping: true);
        Assert.Equal(VoiceState.Stopped, voice.Status.State);
        voice.Start();
        Assert.Equal(VoiceState.Playing, voice.Status.State);

        float[] output = new float[Rate * 2 / 10];
        engine.RenderOffline(output);
        double rms = Rms(output, channel: 0, skip: 1000);
        Assert.InRange(rms, 0.124, 0.126); // 0.5 * 0.5 * -3 dB pan / sqrt 2

        voice.Pause();
        Assert.Equal(VoiceState.Paused, voice.Status.State);
        voice.Seek(0.05);
        Assert.Equal(0.05, voice.Status.PositionSeconds, 6);
        voice.Stop();
        Assert.Equal(new VoiceStatus(VoiceState.Stopped, 0), voice.Status);

        EngineStats stats = engine.GetStats();
        Assert.Equal(OutputKind.None, stats.Output);
        Assert.Equal(Rate, stats.SampleRate);
        Assert.Equal(1, stats.AllocatedVoices);
        Assert.True(stats.BlocksRendered > 0);
    }

    [Fact]
    public void Events_carry_fade_tokens_and_voice_ends()
    {
        using AudioEngine engine = CreateEngine();
        using AudioAsset asset = engine.CreateAsset(Wav(Sine(1000, Rate, Rate / 20, 0.5), Rate), "blip.wav");
        using Voice fading = engine.CreateVoice(asset, looping: true);
        using Voice oneShot = engine.CreateVoice(asset);
        fading.Start();
        oneShot.Start();
        fading.FadeTo(0f, 0.02f, token: 77, stopWhenDone: true);
        engine.RenderOffline(new float[Rate / 5 * 2]);

        Span<EngineEvent> events = new EngineEvent[16];
        int count = engine.PollEvents(events);
        Assert.Contains(new EngineEvent(EngineEventType.FadeDone, fading.Handle, 77, false), events[..count].ToArray());
        Assert.Contains(new EngineEvent(EngineEventType.VoiceEnded, oneShot.Handle, 0, false), events[..count].ToArray());
        Assert.Equal(VoiceState.Stopped, fading.Status.State);
        Assert.Equal(VoiceState.Stopped, oneShot.Status.State);
    }

    [Fact]
    public void Native_errors_surface_as_exceptions()
    {
        using AudioEngine engine = CreateEngine();
        NativeException decode = Assert.Throws<NativeException>(() => engine.CreateAsset("not audio"u8));
        Assert.Equal("Decode", decode.Result);

        using AudioAsset asset = engine.CreatePcmAsset(Sine(440, Rate, 480, 0.5), 1, Rate);
        Voice voice = engine.CreateVoice(asset);
        voice.Dispose();
        NativeException stale = Assert.Throws<NativeException>(() => voice.Start());
        Assert.Equal("InvalidHandle", stale.Result);
        voice.Dispose(); // idempotent

        Assert.Throws<NativeException>(() => engine.CreateVoice(asset, pitch: 0f));
        Assert.Throws<ArgumentException>(() => engine.RenderOffline(new float[3]));
    }

    [Fact]
    public void A_disposed_engine_rejects_calls_and_voices_dispose_quietly()
    {
        AudioEngine engine = CreateEngine();
        using AudioAsset asset = engine.CreatePcmAsset(Sine(440, Rate, 480, 0.5), 1, Rate);
        Voice voice = engine.CreateVoice(asset);
        engine.Dispose();

        Assert.True(engine.IsDisposed);
        Assert.Throws<ObjectDisposedException>(() => voice.Start());
        Assert.Throws<ObjectDisposedException>(() => engine.GetStats());
        voice.Dispose();
        Assert.Equal(1, asset.Info.Channels); // assets outlive the engine
    }

    [Fact]
    public void Offline_output_format_can_change()
    {
        using AudioEngine engine = CreateEngine();
        engine.OpenOffline(sampleRate: 44100, channels: 6);
        EngineStats stats = engine.GetStats();
        Assert.Equal(44100, stats.SampleRate);
        Assert.Equal(6, stats.Channels);
        engine.RenderOffline(new float[6 * 256]);
        Assert.Throws<ArgumentException>(() => engine.RenderOffline(new float[4]));
    }

    [Fact]
    public void Device_enumeration_is_well_formed()
    {
        using AudioEngine engine = CreateEngine();
        IReadOnlyList<AudioDevice> devices;
        try
        {
            devices = engine.EnumerateDevices();
        }
        catch (NativeException ex) when (ex.Result == "Device")
        {
            Assert.Skip("no audio backend on this machine");
            return;
        }

        Assert.All(devices, d => Assert.False(string.IsNullOrEmpty(d.Name)));
        Assert.All(devices, d => Assert.Equal(512, d.Id.Length));
        Assert.True(devices.Count(d => d.IsDefault) <= 1);
    }

    private static AudioEngine CreateEngine()
    {
        NativeTestEnvironment.RequireNatives();
        return AudioEngine.Create(new EngineOptions { RayTracer = RayTracer.Steam }, null);
    }

    internal static short[] Sine(double frequency, int rate, int frames, double amplitude)
    {
        short[] samples = new short[frames];
        for (int i = 0; i < frames; i++)
        {
            samples[i] = (short)Math.Round(amplitude * 32767 * Math.Sin(2 * Math.PI * frequency * i / rate));
        }

        return samples;
    }

    internal static byte[] Wav(short[] mono, int rate)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write("RIFF"u8);
        writer.Write(36 + mono.Length * 2);
        writer.Write("WAVEfmt "u8);
        writer.Write(16);
        writer.Write((short)1);
        writer.Write((short)1);
        writer.Write(rate);
        writer.Write(rate * 2);
        writer.Write((short)2);
        writer.Write((short)16);
        writer.Write("data"u8);
        writer.Write(mono.Length * 2);
        foreach (short sample in mono)
        {
            writer.Write(sample);
        }

        writer.Flush();
        return stream.ToArray();
    }

    private static double Rms(float[] interleaved, int channel, int skip)
    {
        double sum = 0;
        int n = 0;
        for (int i = (skip * 2) + channel; i < interleaved.Length; i += 2)
        {
            sum += interleaved[i] * (double)interleaved[i];
            n++;
        }

        return Math.Sqrt(sum / n);
    }
}

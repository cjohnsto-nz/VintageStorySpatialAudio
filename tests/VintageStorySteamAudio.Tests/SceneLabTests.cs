using SceneLab;
using VintageStorySteamAudio.Native;

namespace VintageStorySteamAudio.Tests;

[Collection(NativeEngineGroup.Name)]
public sealed class SceneLabTests
{
    private static string ScenarioPath(string name) =>
        Path.Combine(NativeTestEnvironment.RepoRoot(), "tools", "SceneLab", "scenarios", name);

    [Fact]
    public void The_smoke_scenario_renders_and_meets_its_expectations()
    {
        NativeTestEnvironment.RequireNatives();
        string path = ScenarioPath("phase1-smoke.json");
        Scenario scenario = Scenario.Load(path);

        ScenarioResult result = ScenarioRunner.Run(scenario, Path.GetDirectoryName(path)!);

        ScenarioMetrics m = result.Metrics;
        Assert.Empty(m.Failures);
        Assert.Equal(scenario.DurationSeconds * scenario.SampleRate, m.Frames);
        Assert.Equal(m.Frames * m.Channels, result.Samples.Length);
        Assert.Equal(5, m.Voices);
        // Scripted timeline: two noise bursts and the stereo tone end, then the fade completes.
        Assert.Equal(3, m.Events.Count(e => e.Type == EngineEventType.VoiceEnded));
        EventRecord fade = Assert.Single(m.Events, e => e.Type == EngineEventType.FadeDone);
        Assert.InRange(fade.Time, 4.99, 5.01);
    }

    [Fact]
    public void Wav_output_is_a_valid_float_wave_file()
    {
        string file = Path.Combine(Path.GetTempPath(), $"scenelab-{Guid.NewGuid():N}.wav");
        try
        {
            WavFile.WriteFloat(file, [0.5f, -0.5f, 0.25f, -0.25f], channels: 2, sampleRate: 48000);
            byte[] bytes = File.ReadAllBytes(file);
            Assert.Equal(44 + 16, bytes.Length);
            Assert.Equal("RIFF"u8.ToArray(), bytes[..4]);
            Assert.Equal((short)3, BitConverter.ToInt16(bytes, 20)); // IEEE float
            Assert.Equal((short)2, BitConverter.ToInt16(bytes, 22));
            Assert.Equal(48000, BitConverter.ToInt32(bytes, 24));
            Assert.Equal(-0.25f, BitConverter.ToSingle(bytes, 56));
        }
        finally
        {
            File.Delete(file);
        }
    }

    [Fact]
    public void Scenarios_parse_with_comments_enums_and_defaults()
    {
        foreach (string file in Directory.GetFiles(Path.GetDirectoryName(ScenarioPath("x"))!, "*.json"))
        {
            Scenario scenario = Scenario.Load(file);
            Assert.NotEmpty(scenario.Voices);
            Assert.All(scenario.Voices, v => Assert.True(scenario.Assets.ContainsKey(v.Asset), $"{file}: unknown asset {v.Asset}"));
        }

        Scenario smoke = Scenario.Load(ScenarioPath("phase1-smoke.json"));
        Assert.Equal(ToneShape.Saw, smoke.Assets["saw110"].Tone!.Shape);
        Assert.Equal(AudioBus.Weather, smoke.Voices[3].Bus);
        Assert.Null(smoke.Voices[0].Actions[0].Pause);
    }
}

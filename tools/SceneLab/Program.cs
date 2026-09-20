using System.Globalization;
using System.Text.Json;
using SceneLab;
using VintageStorySpatialAudio.Native;

// SceneLab <scenario.json>... [--out <dir>] [--native <folder with vsaudio + phonon>]
//
// Renders each scenario offline to <out>/<name>.wav and <out>/<name>.metrics.json.
// Exit codes: 0 = rendered and every expectation met, 1 = an expectation failed or a render
// error occurred, 2 = usage error.

var scenarios = new List<string>();
string outDirectory = "scenelab-out";
string? nativeDirectory = null;
for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--out" when i + 1 < args.Length:
            outDirectory = args[++i];
            break;
        case "--native" when i + 1 < args.Length:
            nativeDirectory = args[++i];
            break;
        default:
            if (args[i].StartsWith("--", StringComparison.Ordinal))
            {
                Console.Error.WriteLine($"Unknown or incomplete option '{args[i]}'.");
                return 2;
            }

            scenarios.Add(args[i]);
            break;
    }
}

if (scenarios.Count == 0)
{
    Console.Error.WriteLine("Usage: SceneLab <scenario.json>... [--out <dir>] [--native <native folder>]");
    return 2;
}

nativeDirectory ??= FindNatives();
NativeLibraryResolver.Register(nativeDirectory);
Directory.CreateDirectory(outDirectory);

bool ok = true;
foreach (string path in scenarios)
{
    try
    {
        Scenario scenario = Scenario.Load(path);
        ScenarioResult result = ScenarioRunner.Run(scenario, Path.GetDirectoryName(Path.GetFullPath(path))!, new ConsoleLog());
        ScenarioMetrics m = result.Metrics;

        string wav = Path.Combine(outDirectory, scenario.Name + ".wav");
        WavFile.WriteFloat(wav, result.Samples, m.Channels, m.SampleRate);
        File.WriteAllText(Path.Combine(outDirectory, scenario.Name + ".metrics.json"), JsonSerializer.Serialize(m, Scenario.JsonOptions));

        string levels = string.Join(", ", m.ChannelLevels.Select(
            c => string.Create(CultureInfo.InvariantCulture, $"{c.PeakDbfs:0.0}/{c.RmsDbfs:0.0}")));
        Console.WriteLine(string.Create(
            CultureInfo.InvariantCulture,
            $"{scenario.Name}: {m.Frames / (double)m.SampleRate:0.##} s, {m.Voices} voices, peak/RMS dBFS [{levels}], " +
            $"block p99 {m.Render.P99Us:0} us ({m.Render.P99Load:P1} of {m.Render.BlockPeriodUs:0} us), " +
            $"underruns {m.StreamUnderruns}, limiter {m.LimiterPeakReductionDb:0.0} dB -> {wav}"));
        foreach (string failure in m.Failures)
        {
            Console.WriteLine($"  FAILED: {failure}");
            ok = false;
        }
    }
    catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException or NativeException
                                   or ArgumentException or InvalidOperationException)
    {
        Console.Error.WriteLine($"{path}: {ex.Message}");
        ok = false;
    }
}

return ok ? 0 : 1;

static string FindNatives()
{
    // A repo checkout (artifacts/native/<rid>), else next to the tool.
    var directory = new DirectoryInfo(AppContext.BaseDirectory);
    while (directory is not null)
    {
        string candidate = Path.Combine(directory.FullName, "artifacts", "native", NativeLibraryResolver.RuntimeFolderName);
        if (Directory.Exists(candidate))
        {
            return candidate;
        }

        directory = directory.Parent;
    }

    return Path.Combine(AppContext.BaseDirectory, "native", NativeLibraryResolver.RuntimeFolderName);
}

internal sealed class ConsoleLog : IEngineLog
{
    public void Write(EngineLogLevel level, string message)
    {
        if (level >= EngineLogLevel.Warning)
        {
            Console.Error.WriteLine($"  [{level}] {message}");
        }
    }
}

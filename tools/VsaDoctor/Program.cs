using System.Reflection;
using System.Runtime.Loader;
using VintageStorySteamAudio.Diagnostics;
using VintageStorySteamAudio.Native;
using VintageStorySteamAudio.Platform;

// VsaDoctor --game <Vintage Story folder> [--native <folder with vsaudio + phonon>] [--targets-only]
//
// Exit codes: 0 = everything passed, 1 = a check failed, 2 = usage error.

string? gameDirectory = null;
string? nativeDirectory = null;
bool targetsOnly = false;

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--game" when i + 1 < args.Length:
            gameDirectory = args[++i];
            break;
        case "--native" when i + 1 < args.Length:
            nativeDirectory = args[++i];
            break;
        case "--targets-only":
            targetsOnly = true;
            break;
        default:
            Console.Error.WriteLine($"Unknown or incomplete argument '{args[i]}'.");
            Console.Error.WriteLine("Usage: VsaDoctor --game <Vintage Story folder> [--native <native folder>] [--targets-only]");
            return 2;
    }
}

gameDirectory ??= Environment.GetEnvironmentVariable("VINTAGE_STORY");
if (gameDirectory is null || !File.Exists(Path.Combine(gameDirectory, "VintagestoryLib.dll")))
{
    Console.Error.WriteLine("Pass --game <folder containing VintagestoryLib.dll> or set VINTAGE_STORY.");
    return 2;
}

gameDirectory = Path.GetFullPath(gameDirectory);
string[] probeDirectories = [gameDirectory, Path.Combine(gameDirectory, "Lib"), Path.Combine(gameDirectory, "Mods")];
AssemblyLoadContext.Default.Resolving += (context, name) =>
{
    foreach (string directory in probeDirectories)
    {
        string candidate = Path.Combine(directory, name.Name + ".dll");
        if (File.Exists(candidate))
        {
            return context.LoadFromAssemblyPath(candidate);
        }
    }

    return null;
};

Assembly lib = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(gameDirectory, "VintagestoryLib.dll"));
Assembly api = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(gameDirectory, "VintagestoryAPI.dll"));

VerificationReport verification = PatchTargetVerifier.Verify(
    new GameAssemblies(lib, api), AudioPatchTargets.Members, AudioPatchTargets.Invariants);

var report = new StatusReport { Verification = verification };
if (!targetsOnly)
{
    nativeDirectory ??= Path.Combine(AppContext.BaseDirectory, "native", NativeLibraryResolver.RuntimeFolderName);
    try
    {
        NativeLibraryResolver.Register(nativeDirectory);
        EngineVersion version = AudioEngine.GetVersion();
        using AudioEngine engine = AudioEngine.Create(new EngineOptions(), new ConsoleEngineLog());
        report = report with { Version = version, Engine = engine.Info, SelfTest = engine.RunSelfTest() };
    }
    catch (Exception ex)
    {
        report = report with { EngineError = $"{ex.GetType().Name}: {ex.Message}" };
    }
}

Console.WriteLine($"Game: {gameDirectory} ({lib.GetName().Name} {lib.GetName().Version})");
Console.WriteLine(report.Render(includePassingTargets: true));

bool ok = verification.AllPassed && (targetsOnly || report.TakeoverPossible);
return ok ? 0 : 1;

internal sealed class ConsoleEngineLog : IEngineLog
{
    public void Write(EngineLogLevel level, string message) => Console.WriteLine($"  [{level}] {message}");
}

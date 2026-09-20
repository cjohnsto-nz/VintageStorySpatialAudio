using System.Reflection;

namespace VintageStorySpatialAudio.Platform;

/// <summary>The game assemblies that patch targets live in.</summary>
public sealed class GameAssemblies
{
    public const string LibAssemblyName = "VintagestoryLib";
    public const string ApiAssemblyName = "VintagestoryAPI";

    public GameAssemblies(Assembly lib, Assembly api)
    {
        Lib = lib ?? throw new ArgumentNullException(nameof(lib));
        Api = api ?? throw new ArgumentNullException(nameof(api));
    }

    public Assembly Lib { get; }

    public Assembly Api { get; }

    /// <summary>The game assemblies already loaded in this process (i.e. inside the game).</summary>
    public static GameAssemblies FromCurrentProcess()
    {
        Assembly? lib = null;
        Assembly? api = null;
        foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            string? name = assembly.GetName().Name;
            if (name == LibAssemblyName) lib ??= assembly;
            else if (name == ApiAssemblyName) api ??= assembly;
        }

        return new GameAssemblies(
            lib ?? throw new InvalidOperationException($"{LibAssemblyName} is not loaded in this process."),
            api ?? throw new InvalidOperationException($"{ApiAssemblyName} is not loaded in this process."));
    }

    /// <summary>Finds a type by full name in VintagestoryLib, then VintagestoryAPI.</summary>
    public Type? FindType(string fullName) => Lib.GetType(fullName, throwOnError: false) ?? Api.GetType(fullName, throwOnError: false);

    /// <summary>All types in VintagestoryLib that could be loaded.</summary>
    public IEnumerable<Type> LibTypes()
    {
        try
        {
            return Lib.GetTypes();
        }
        catch (ReflectionTypeLoadException ex)
        {
            // Types whose dependencies are unavailable (e.g. in offline tooling) are skipped.
            return ex.Types.Where(t => t is not null)!;
        }
    }
}

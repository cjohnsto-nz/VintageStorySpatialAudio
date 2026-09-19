namespace VintageStorySteamAudio.Platform;

public enum TargetMemberKind
{
    Method,
    Property,
    Field,
}

/// <summary>
/// A game member the mod depends on. Signatures are written as C#-style type names with
/// namespaces (e.g. <c>System.Collections.Generic.IList&lt;System.String&gt;</c>), matching
/// <see cref="TypeNames.Format"/>.
/// </summary>
/// <param name="Id">Stable identifier used in reports.</param>
/// <param name="TypeName">Full name of the declaring type.</param>
/// <param name="Kind">Member kind.</param>
/// <param name="MemberName">Member name.</param>
/// <param name="Type">Return type (methods), property type or field type.</param>
/// <param name="Parameters">Parameter types (methods only).</param>
/// <param name="IsStatic">Whether the member is static.</param>
/// <param name="Purpose">Why the mod needs it (shown in reports).</param>
public sealed record PatchTarget(
    string Id,
    string TypeName,
    TargetMemberKind Kind,
    string MemberName,
    string Type,
    IReadOnlyList<string> Parameters,
    bool IsStatic,
    string Purpose)
{
    public string Describe() => Kind switch
    {
        TargetMemberKind.Method => $"{(IsStatic ? "static " : string.Empty)}{Type} {TypeName}.{MemberName}({string.Join(", ", Parameters)})",
        _ => $"{(IsStatic ? "static " : string.Empty)}{Type} {TypeName}.{MemberName}",
    };
}

/// <summary>A structural fact about the game the mod relies on, beyond individual members.</summary>
/// <param name="Id">Stable identifier used in reports.</param>
/// <param name="Description">What must hold.</param>
/// <param name="Check">Returns null when the invariant holds, otherwise a failure message.</param>
public sealed record GameInvariant(string Id, string Description, Func<GameAssemblies, string?> Check);

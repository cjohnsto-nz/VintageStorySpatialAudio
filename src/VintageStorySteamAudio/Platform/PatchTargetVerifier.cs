using System.Reflection;

namespace VintageStorySteamAudio.Platform;

public enum VerificationStatus
{
    Ok,
    TypeMissing,
    MemberMissing,
    SignatureMismatch,
    Failed,
}

public sealed record VerificationEntry(string Id, string Description, VerificationStatus Status, string? Detail);

public sealed record VerificationReport(IReadOnlyList<VerificationEntry> Entries)
{
    public bool AllPassed => Entries.All(e => e.Status == VerificationStatus.Ok);

    public int PassedCount => Entries.Count(e => e.Status == VerificationStatus.Ok);

    public IEnumerable<VerificationEntry> Failures => Entries.Where(e => e.Status != VerificationStatus.Ok);
}

/// <summary>Checks <see cref="PatchTarget"/>s and <see cref="GameInvariant"/>s against the game's assemblies.</summary>
public static class PatchTargetVerifier
{
    private const BindingFlags AnyMember =
        BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

    public static VerificationReport Verify(GameAssemblies game, IEnumerable<PatchTarget> targets, IEnumerable<GameInvariant> invariants)
    {
        ArgumentNullException.ThrowIfNull(game);
        var entries = new List<VerificationEntry>();
        entries.AddRange(targets.Select(t => VerifyTarget(game, t)));
        entries.AddRange(invariants.Select(i => VerifyInvariant(game, i)));
        return new VerificationReport(entries);
    }

    private static VerificationEntry VerifyInvariant(GameAssemblies game, GameInvariant invariant)
    {
        try
        {
            string? failure = invariant.Check(game);
            return new VerificationEntry(invariant.Id, invariant.Description, failure is null ? VerificationStatus.Ok : VerificationStatus.Failed, failure);
        }
        catch (Exception ex)
        {
            return new VerificationEntry(invariant.Id, invariant.Description, VerificationStatus.Failed, ex.GetType().Name + ": " + ex.Message);
        }
    }

    private static VerificationEntry VerifyTarget(GameAssemblies game, PatchTarget target)
    {
        VerificationEntry Result(VerificationStatus status, string? detail = null) => new(target.Id, target.Describe(), status, detail);

        try
        {
            Type? type = game.FindType(target.TypeName);
            if (type is null)
            {
                // Game updates sometimes move types between namespaces; point at candidates.
                string simpleName = target.TypeName[(target.TypeName.LastIndexOf('.') + 1)..];
                string[] candidates = game.LibTypes().Where(t => t.Name == simpleName).Select(t => t.FullName ?? t.Name).ToArray();
                return Result(
                    VerificationStatus.TypeMissing,
                    $"type {target.TypeName} not found" + (candidates.Length > 0 ? $"; same name exists as: {string.Join(", ", candidates)}" : string.Empty));
            }

            MemberInfo[] members = type.GetMember(target.MemberName, AnyMember);
            if (members.Length == 0)
            {
                return Result(VerificationStatus.MemberMissing, $"{type.FullName} has no member '{target.MemberName}'");
            }

            return target.Kind switch
            {
                TargetMemberKind.Method => MatchMethod(target, members.OfType<MethodInfo>().ToList(), Result),
                TargetMemberKind.Property => MatchProperty(target, members.OfType<PropertyInfo>().ToList(), Result),
                TargetMemberKind.Field => MatchField(target, members.OfType<FieldInfo>().ToList(), Result),
                _ => Result(VerificationStatus.Failed, "unknown member kind"),
            };
        }
        catch (Exception ex)
        {
            return Result(VerificationStatus.Failed, ex.GetType().Name + ": " + ex.Message);
        }
    }

    private static VerificationEntry MatchMethod(PatchTarget target, List<MethodInfo> methods, Func<VerificationStatus, string?, VerificationEntry> result)
    {
        if (methods.Count == 0)
        {
            return result(VerificationStatus.MemberMissing, "member exists but is not a method");
        }

        foreach (MethodInfo method in methods)
        {
            string[] parameters = method.GetParameters().Select(p => TypeNames.Format(p.ParameterType)).ToArray();
            if (method.IsStatic == target.IsStatic
                && TypeNames.Format(method.ReturnType) == target.Type
                && parameters.SequenceEqual(target.Parameters, StringComparer.Ordinal))
            {
                return result(VerificationStatus.Ok, null);
            }
        }

        string found = string.Join(" | ", methods.Select(m =>
            $"{(m.IsStatic ? "static " : string.Empty)}{TypeNames.Format(m.ReturnType)} {m.Name}({string.Join(", ", m.GetParameters().Select(p => TypeNames.Format(p.ParameterType)))})"));
        return result(VerificationStatus.SignatureMismatch, "found: " + found);
    }

    private static VerificationEntry MatchProperty(PatchTarget target, List<PropertyInfo> properties, Func<VerificationStatus, string?, VerificationEntry> result)
    {
        if (properties.Count == 0)
        {
            return result(VerificationStatus.MemberMissing, "member exists but is not a property");
        }

        PropertyInfo property = properties[0];
        bool isStatic = (property.GetMethod ?? property.SetMethod)?.IsStatic ?? false;
        string type = TypeNames.Format(property.PropertyType);
        return type == target.Type && isStatic == target.IsStatic
            ? result(VerificationStatus.Ok, null)
            : result(VerificationStatus.SignatureMismatch, $"found: {(isStatic ? "static " : string.Empty)}{type}");
    }

    private static VerificationEntry MatchField(PatchTarget target, List<FieldInfo> fields, Func<VerificationStatus, string?, VerificationEntry> result)
    {
        if (fields.Count == 0)
        {
            return result(VerificationStatus.MemberMissing, "member exists but is not a field");
        }

        FieldInfo field = fields[0];
        string type = TypeNames.Format(field.FieldType);
        return type == target.Type && field.IsStatic == target.IsStatic
            ? result(VerificationStatus.Ok, null)
            : result(VerificationStatus.SignatureMismatch, $"found: {(field.IsStatic ? "static " : string.Empty)}{type}");
    }
}

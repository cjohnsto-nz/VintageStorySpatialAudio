using System.Text;

namespace VintageStorySpatialAudio.Platform;

/// <summary>Formats types as readable, namespace-qualified C#-style names for signature matching.</summary>
public static class TypeNames
{
    public static string Format(Type type)
    {
        ArgumentNullException.ThrowIfNull(type);

        if (type.IsByRef)
        {
            return "ref " + Format(type.GetElementType()!);
        }

        if (type.IsArray)
        {
            return Format(type.GetElementType()!) + "[" + new string(',', type.GetArrayRank() - 1) + "]";
        }

        if (type.IsGenericParameter)
        {
            return type.Name;
        }

        string name = type.IsNested
            ? Format(type.DeclaringType!) + "+" + StripArity(type.Name)
            : (type.Namespace is null ? string.Empty : type.Namespace + ".") + StripArity(type.Name);

        if (!type.IsGenericType)
        {
            return name;
        }

        var builder = new StringBuilder(name).Append('<');
        Type[] arguments = type.GetGenericArguments();
        for (int i = 0; i < arguments.Length; i++)
        {
            if (i > 0)
            {
                builder.Append(", ");
            }

            builder.Append(Format(arguments[i]));
        }

        return builder.Append('>').ToString();
    }

    private static string StripArity(string name)
    {
        int tick = name.IndexOf('`', StringComparison.Ordinal);
        return tick < 0 ? name : name[..tick];
    }
}

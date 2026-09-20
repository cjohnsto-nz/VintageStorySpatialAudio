using VintageStorySpatialAudio.Platform;

namespace VintageStorySpatialAudio.Tests;

public sealed class TypeNamesTests
{
    private sealed class Nested;

    [Theory]
    [InlineData(typeof(void), "System.Void")]
    [InlineData(typeof(float), "System.Single")]
    [InlineData(typeof(byte[]), "System.Byte[]")]
    [InlineData(typeof(int[,]), "System.Int32[,]")]
    [InlineData(typeof(IList<string>), "System.Collections.Generic.IList<System.String>")]
    [InlineData(typeof(Dictionary<string, List<int>>), "System.Collections.Generic.Dictionary<System.String, System.Collections.Generic.List<System.Int32>>")]
    [InlineData(typeof(int?), "System.Nullable<System.Int32>")]
    public void Formats_types_as_namespace_qualified_csharp_names(Type type, string expected) =>
        Assert.Equal(expected, TypeNames.Format(type));

    [Fact]
    public void Formats_nested_types_with_a_plus() =>
        Assert.Equal("VintageStorySpatialAudio.Tests.TypeNamesTests+Nested", TypeNames.Format(typeof(Nested)));

    [Fact]
    public void Formats_by_ref_parameters() =>
        Assert.Equal("ref System.Int32", TypeNames.Format(typeof(int).MakeByRefType()));
}

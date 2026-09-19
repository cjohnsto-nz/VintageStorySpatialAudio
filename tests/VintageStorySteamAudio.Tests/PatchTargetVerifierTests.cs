using VintageStorySteamAudio.Platform;

namespace VintageStorySteamAudio.Tests.Fakes
{
    // Stand-ins for game types, so the verifier's matching rules can be tested precisely.
#pragma warning disable CA1822, CA1812, IDE0051, IDE0060, CS0169, CS0649
    internal abstract class FakePlatformBase
    {
        public abstract void StartAudio();
    }

    internal sealed class FakePlatform : FakePlatformBase
    {
        private static object? registry;
        private int counter;

        public float MasterLevel { get; set; }

        public override void StartAudio() { }

        public IList<string> Create(string name, int count) => [];

        public IList<string> Create(string name) => [];

        private static void Hidden(float value) { }
    }
#pragma warning restore CA1822, CA1812, IDE0051, IDE0060, CS0169, CS0649
}

namespace VintageStorySteamAudio.Tests
{
    public sealed class PatchTargetVerifierTests
    {
        private const string Fake = "VintageStorySteamAudio.Tests.Fakes.FakePlatform";
        private static readonly GameAssemblies Game = new(typeof(PatchTargetVerifierTests).Assembly, typeof(PatchTargetVerifierTests).Assembly);

        private static VerificationEntry VerifyOne(PatchTarget target) =>
            Assert.Single(PatchTargetVerifier.Verify(Game, [target], []).Entries);

        private static PatchTarget Method(string name, string returns, string[] parameters, bool isStatic = false) =>
            new("t", Fake, TargetMemberKind.Method, name, returns, parameters, isStatic, "test");

        [Fact]
        public void Matches_an_overload_by_exact_signature()
        {
            VerificationEntry entry = VerifyOne(Method("Create", "System.Collections.Generic.IList<System.String>", ["System.String", "System.Int32"]));
            Assert.Equal(VerificationStatus.Ok, entry.Status);
        }

        [Fact]
        public void Reports_signature_mismatch_with_the_overloads_that_exist()
        {
            VerificationEntry entry = VerifyOne(Method("Create", "System.Collections.Generic.IList<System.String>", ["System.Int32"]));
            Assert.Equal(VerificationStatus.SignatureMismatch, entry.Status);
            Assert.Contains("Create(System.String, System.Int32)", entry.Detail, StringComparison.Ordinal);
            Assert.Contains("Create(System.String)", entry.Detail, StringComparison.Ordinal);
        }

        [Fact]
        public void Finds_private_static_methods_and_checks_staticness()
        {
            Assert.Equal(VerificationStatus.Ok, VerifyOne(Method("Hidden", "System.Void", ["System.Single"], isStatic: true)).Status);
            Assert.Equal(VerificationStatus.SignatureMismatch, VerifyOne(Method("Hidden", "System.Void", ["System.Single"], isStatic: false)).Status);
        }

        [Fact]
        public void Checks_fields_and_properties()
        {
            Assert.Equal(VerificationStatus.Ok, VerifyOne(new PatchTarget("f", Fake, TargetMemberKind.Field, "registry", "System.Object", [], true, "test")).Status);
            Assert.Equal(VerificationStatus.Ok, VerifyOne(new PatchTarget("f", Fake, TargetMemberKind.Field, "counter", "System.Int32", [], false, "test")).Status);
            Assert.Equal(VerificationStatus.Ok, VerifyOne(new PatchTarget("p", Fake, TargetMemberKind.Property, "MasterLevel", "System.Single", [], false, "test")).Status);
            Assert.Equal(VerificationStatus.SignatureMismatch, VerifyOne(new PatchTarget("p", Fake, TargetMemberKind.Property, "MasterLevel", "System.Double", [], false, "test")).Status);
        }

        [Fact]
        public void Reports_missing_members()
        {
            VerificationEntry entry = VerifyOne(Method("DoesNotExist", "System.Void", []));
            Assert.Equal(VerificationStatus.MemberMissing, entry.Status);
        }

        [Fact]
        public void Reports_a_moved_type_with_its_new_location()
        {
            var target = new PatchTarget("moved", "Some.Old.Namespace.FakePlatform", TargetMemberKind.Method, "StartAudio", "System.Void", [], false, "test");
            VerificationEntry entry = VerifyOne(target);
            Assert.Equal(VerificationStatus.TypeMissing, entry.Status);
            Assert.Contains(Fake, entry.Detail, StringComparison.Ordinal);
        }

        [Fact]
        public void Invariant_failures_and_exceptions_are_reported_not_thrown()
        {
            GameInvariant failing = new("fails", "always fails", _ => "nope");
            GameInvariant throwing = new("throws", "always throws", _ => throw new InvalidOperationException("boom"));
            GameInvariant passing = new("passes", "always passes", _ => null);

            VerificationReport report = PatchTargetVerifier.Verify(Game, [], [failing, throwing, passing]);

            Assert.False(report.AllPassed);
            Assert.Equal(1, report.PassedCount);
            Assert.Contains(report.Failures, e => e.Id == "throws" && e.Detail!.Contains("boom", StringComparison.Ordinal));
        }
    }
}

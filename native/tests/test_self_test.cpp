#include "engine_fixture.hpp"

using namespace vsa_test;

namespace {

void require_self_test_passes(vsa_engine* engine) {
    vsa_self_test_report report{};
    report.struct_size = sizeof report;
    REQUIRE(vsa_engine_run_self_test(engine, &report) == VSA_OK);
    MESSAGE("occlusion through wall = " << report.occlusion_through_wall
            << ", clear path = " << report.occlusion_clear_path << ", " << report.elapsed_ms << " ms");
    CHECK(static_cast<double>(report.occlusion_through_wall) == doctest::Approx(0.0).epsilon(0.01));
    CHECK(static_cast<double>(report.occlusion_clear_path) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(report.passed == 1u);
}

}  // namespace

TEST_CASE("self-test passes with Steam Audio's built-in ray tracer") {
    ScopedEngine scoped(make_config(VSA_RAY_TRACER_STEAM));
    REQUIRE(scoped.result == VSA_OK);
    require_self_test_passes(scoped.engine);
}

TEST_CASE("self-test passes with Embree where Embree is available") {
    vsa_engine* engine = nullptr;
    auto config = make_config(VSA_RAY_TRACER_EMBREE);
    const vsa_result result = vsa_engine_create(&config, &engine);
    if (result == VSA_ERROR_UNSUPPORTED) {
        MESSAGE("Embree is not available on this platform; skipped (" << vsa_get_last_error() << ")");
        return;
    }
    REQUIRE(result == VSA_OK);
    require_self_test_passes(engine);
    vsa_engine_destroy(engine);
}

TEST_CASE("self-test can run repeatedly on one engine") {
    ScopedEngine scoped(make_config(VSA_RAY_TRACER_AUTO));
    REQUIRE(scoped.result == VSA_OK);
    for (int i = 0; i < 3; ++i) {
        require_self_test_passes(scoped.engine);
    }
}

TEST_CASE("self-test rejects bad arguments") {
    ScopedEngine scoped(make_config(VSA_RAY_TRACER_STEAM));
    REQUIRE(scoped.result == VSA_OK);
    vsa_self_test_report report{};
    report.struct_size = 3;
    CHECK(vsa_engine_run_self_test(scoped.engine, &report) == VSA_ERROR_ABI_MISMATCH);
    CHECK(vsa_engine_run_self_test(scoped.engine, nullptr) == VSA_ERROR_INVALID_ARGUMENT);
    report.struct_size = sizeof report;
    CHECK(vsa_engine_run_self_test(nullptr, &report) == VSA_ERROR_INVALID_ARGUMENT);
}

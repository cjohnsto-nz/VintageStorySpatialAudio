set(DOCTEST_INCLUDE_DIR "${VSA_THIRD_PARTY_DIR}/doctest")
if(NOT EXISTS "${DOCTEST_INCLUDE_DIR}/doctest/doctest.h")
    message(FATAL_ERROR "doctest not found; run scripts/fetch-deps first (or configure with -DVSA_BUILD_TESTS=OFF)")
endif()

# libvorbis's encoder, test-only: tests encode their own Ogg files so no binary fixtures are
# committed. Never linked into the shipped library.
add_library(vsa_vorbisenc STATIC "${VORBIS_DIR}/lib/vorbisenc.c")
target_include_directories(vsa_vorbisenc PRIVATE "${VORBIS_DIR}/lib")
target_link_libraries(vsa_vorbisenc PUBLIC vsa_vorbis)
if(MSVC)
    target_compile_definitions(vsa_vorbisenc PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()
vsa_disable_warnings(vsa_vorbisenc)

add_library(vsa_test_support STATIC support/signals.cpp support/signals.hpp)
target_include_directories(vsa_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vsa_test_support PRIVATE vsa_vorbisenc)
vsa_enable_warnings(vsa_test_support)

function(vsa_place_runtime target)
    if(APPLE)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    vsa_copy_phonon_runtime(${target})
endfunction()

# ---------------------------------------------------------------------------
# vsaudio_tests: the public C ABI only, exactly like the managed side uses it.
# ---------------------------------------------------------------------------
add_executable(vsaudio_tests
    test_main.cpp
    test_api.cpp
    test_assets.cpp
    test_beds.cpp
    test_direct.cpp
    test_header_c.c
    test_output.cpp
    test_pathing.cpp
    test_reflections.cpp
    test_scene.cpp
    test_self_test.cpp
    test_spatial.cpp
    test_streaming.cpp
    test_voices.cpp)
target_include_directories(vsaudio_tests SYSTEM PRIVATE "${DOCTEST_INCLUDE_DIR}")
target_link_libraries(vsaudio_tests PRIVATE vsaudio vsa_test_support)
# A real SOFA HRTF (MIT KEMAR, fetched by fetch-deps) for the SOFA loading tests.
target_compile_definitions(vsaudio_tests PRIVATE
    VSA_TEST_SOFA_PATH="${VSA_THIRD_PARTY_DIR}/sofa/mit_kemar_normal_pinna.sofa")
vsa_enable_warnings(vsaudio_tests)
vsa_place_runtime(vsaudio_tests)
# Put vsaudio next to the test binary.
add_custom_command(TARGET vsaudio_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:vsaudio>" "$<TARGET_FILE_DIR:vsaudio_tests>"
    VERBATIM)
add_test(NAME vsaudio_tests COMMAND vsaudio_tests)

# ---------------------------------------------------------------------------
# vsaudio_core_tests: engine internals (DSP golden tests, rings, the zero-allocation render
# check and the voice load test). Links the core statically, so its operator new replacement
# sees every allocation the engine makes, on every platform.
# ---------------------------------------------------------------------------
add_executable(vsaudio_core_tests
    test_main.cpp
    core/alloc_counter.cpp
    core/alloc_counter.hpp
    core/test_bed_panner.cpp
    core/test_channel_layout.cpp
    core/test_gain_ramp.cpp
    core/test_late_reverb.cpp
    core/test_limiter.cpp
    core/test_mesher.cpp
    core/test_pathing_bake.cpp
    core/test_render_budget.cpp
    core/test_resampler.cpp
    core/test_speaker_decoder.cpp
    core/test_spherical_harmonics.cpp
    core/test_reflection_calibration.cpp
    core/test_spsc_ring.cpp
    core/test_transmission.cpp
    core/test_embree_scene_edits.cpp
    core/test_vbap.cpp
    core/test_world_scene.cpp)
target_include_directories(vsaudio_core_tests SYSTEM PRIVATE "${DOCTEST_INCLUDE_DIR}")
target_link_libraries(vsaudio_core_tests PRIVATE vsaudio_core vsa_test_support)
vsa_enable_warnings(vsaudio_core_tests)
vsa_place_runtime(vsaudio_core_tests)
add_test(NAME vsaudio_core_tests COMMAND vsaudio_core_tests)

set(DOCTEST_INCLUDE_DIR "${VSA_THIRD_PARTY_DIR}/doctest")
if(NOT EXISTS "${DOCTEST_INCLUDE_DIR}/doctest/doctest.h")
    message(FATAL_ERROR "doctest not found; run scripts/fetch-deps first (or configure with -DVSA_BUILD_TESTS=OFF)")
endif()

# Tests link only against the public C ABI, exactly like the managed side does.
add_executable(vsaudio_tests
    test_main.cpp
    test_api.cpp
    test_header_c.c
    test_self_test.cpp)
target_include_directories(vsaudio_tests SYSTEM PRIVATE "${DOCTEST_INCLUDE_DIR}")
target_link_libraries(vsaudio_tests PRIVATE vsaudio)
vsa_enable_warnings(vsaudio_tests)
if(APPLE)
    set_target_properties(vsaudio_tests PROPERTIES BUILD_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(vsaudio_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
# Put vsaudio and phonon next to the test binary.
add_custom_command(TARGET vsaudio_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:vsaudio>" "$<TARGET_FILE_DIR:vsaudio_tests>"
    VERBATIM)
vsa_copy_phonon_runtime(vsaudio_tests)

add_test(NAME vsaudio_tests COMMAND vsaudio_tests)

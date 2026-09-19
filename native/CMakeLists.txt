cmake_minimum_required(VERSION 3.25)

project(vsaudio
    VERSION 0.1.0
    DESCRIPTION "Native audio engine for Vintage Story Steam Audio"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VSA_BUILD_TESTS "Build native unit tests" ON)
option(VSA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(VSA_SANITIZE "Build with address/undefined sanitizers (GCC/Clang, Debug only)" OFF)
set(VSA_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" CACHE PATH
    "Directory populated by scripts/fetch-deps")

include(VsaPlatform)       # sets VSA_RID
include(VsaWarnings)       # vsa_enable_warnings(), vsa_disable_warnings()
include(SteamAudio)        # defines SteamAudio::phonon and vsa_copy_phonon_runtime()
include(Miniaudio)         # vsa_miniaudio
include(Vorbis)            # vsa_ogg, vsa_vorbis

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# vsaudio_core: the whole engine as a static library. The shared library adds
# only the C boundary; the internal unit tests link the core directly.
# ---------------------------------------------------------------------------
add_library(vsaudio_core STATIC
    include/vsaudio.h
    src/engine.cpp
    src/engine.hpp
    src/audio/asset.cpp
    src/audio/asset.hpp
    src/audio/channel_layout.hpp
    src/audio/mixer.cpp
    src/audio/mixer.hpp
    src/audio/spatial.cpp
    src/audio/spatial.hpp
    src/audio/spatial_tier.hpp
    src/audio/stream.cpp
    src/audio/stream.hpp
    src/audio/vbap.cpp
    src/audio/vbap.hpp
    src/audio/voice.hpp
    src/backend/device.cpp
    src/backend/device.hpp
    src/core/error.cpp
    src/core/error.hpp
    src/core/log.cpp
    src/core/latest_value.hpp
    src/core/log.hpp
    src/core/rt_log.cpp
    src/core/rt_log.hpp
    src/core/spsc_ring.hpp
    src/decode/decoders.cpp
    src/decode/decoders.hpp
    src/dsp/gain_ramp.hpp
    src/dsp/high_shelf.hpp
    src/dsp/limiter.cpp
    src/dsp/limiter.hpp
    src/dsp/resampler.cpp
    src/dsp/resampler.hpp
    src/dsp/spherical_harmonics.hpp
    src/steam/ipl_handle.hpp
    src/steam/self_test.cpp
    src/steam/self_test.hpp
    src/steam/steam_context.cpp
    src/steam/steam_context.hpp)
target_include_directories(vsaudio_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vsaudio_core PUBLIC SteamAudio::phonon vsa_miniaudio vsa_vorbis Threads::Threads)
vsa_enable_warnings(vsaudio_core)

# ---------------------------------------------------------------------------
# vsaudio shared library
# ---------------------------------------------------------------------------
set(VSA_BUILD_DESCRIPTION "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, ${VSA_RID}")
configure_file(src/vsaudio_build_info.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/vsaudio_build_info.h" @ONLY)

add_library(vsaudio SHARED src/api.cpp)
target_include_directories(vsaudio
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(vsaudio
    PRIVATE VSA_BUILDING_LIBRARY VSA_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(vsaudio PRIVATE vsaudio_core)
vsa_enable_warnings(vsaudio)

# Find phonon next to vsaudio at runtime on every platform.
if(APPLE)
    set_target_properties(vsaudio PROPERTIES
        BUILD_RPATH "@loader_path"
        INSTALL_RPATH "@loader_path"
        INSTALL_NAME_DIR "@rpath")
elseif(UNIX)
    set_target_properties(vsaudio PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()
# On Windows the managed loader preloads phonon.dll by full path before vsaudio.dll
# (see NativeLibraryResolver.cs), so the dependency resolves without PATH changes.

vsa_copy_phonon_runtime(vsaudio)

# ---------------------------------------------------------------------------
# Install layout == packaged mod layout: <prefix>/<rid>/{vsaudio, phonon}
# ---------------------------------------------------------------------------
install(TARGETS vsaudio
    RUNTIME DESTINATION "${VSA_RID}"
    LIBRARY DESTINATION "${VSA_RID}")
install(FILES "${STEAMAUDIO_RUNTIME_LIBRARY}" DESTINATION "${VSA_RID}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VSA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

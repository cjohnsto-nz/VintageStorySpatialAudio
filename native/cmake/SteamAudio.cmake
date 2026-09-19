# Imports the Steam Audio C API SDK fetched by scripts/fetch-deps.* into
# ${VSA_THIRD_PARTY_DIR}/steamaudio, as the imported target SteamAudio::phonon.
#
# Outputs:
#   SteamAudio::phonon            imported shared library target
#   STEAMAUDIO_RUNTIME_LIBRARY    the file to ship next to vsaudio
#   vsa_copy_phonon_runtime(tgt)  copies the runtime next to a target's output

set(STEAMAUDIO_ROOT "${VSA_THIRD_PARTY_DIR}/steamaudio")
set(STEAMAUDIO_INCLUDE_DIR "${STEAMAUDIO_ROOT}/include")

if(NOT EXISTS "${STEAMAUDIO_INCLUDE_DIR}/phonon.h")
    message(FATAL_ERROR
        "Steam Audio SDK not found at ${STEAMAUDIO_ROOT}.\n"
        "Run scripts/fetch-deps.ps1 (Windows) or scripts/fetch-deps.sh (Linux/macOS) first.")
endif()

if(WIN32)
    set(STEAMAUDIO_RUNTIME_LIBRARY "${STEAMAUDIO_ROOT}/lib/windows-x64/phonon.dll")
    set(_steamaudio_import_lib "${STEAMAUDIO_ROOT}/lib/windows-x64/phonon.lib")
elseif(APPLE)
    set(STEAMAUDIO_RUNTIME_LIBRARY "${STEAMAUDIO_ROOT}/lib/osx/libphonon.dylib")
else()
    set(STEAMAUDIO_RUNTIME_LIBRARY "${STEAMAUDIO_ROOT}/lib/linux-x64/libphonon.so")
endif()

if(NOT EXISTS "${STEAMAUDIO_RUNTIME_LIBRARY}")
    message(FATAL_ERROR "Steam Audio runtime library missing: ${STEAMAUDIO_RUNTIME_LIBRARY}")
endif()

add_library(SteamAudio::phonon SHARED IMPORTED GLOBAL)
set_target_properties(SteamAudio::phonon PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${STEAMAUDIO_INCLUDE_DIR}"
    IMPORTED_LOCATION "${STEAMAUDIO_RUNTIME_LIBRARY}")
if(WIN32)
    set_target_properties(SteamAudio::phonon PROPERTIES IMPORTED_IMPLIB "${_steamaudio_import_lib}")
elseif(NOT APPLE)
    # libphonon.so has SONAME libphonon.so, so the dependency resolves via vsaudio's $ORIGIN rpath.
    set_target_properties(SteamAudio::phonon PROPERTIES IMPORTED_SONAME "libphonon.so")
endif()

# Copies phonon next to the given target's output so tests and local runs resolve it.
function(vsa_copy_phonon_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${STEAMAUDIO_RUNTIME_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

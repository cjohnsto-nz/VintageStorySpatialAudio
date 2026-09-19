# Determines VSA_RID, the runtime identifier used for the packaged native folder
# (native/<rid>/ inside the mod). macOS builds are universal (arm64 + x86_64),
# matching the universal libphonon.dylib shipped by Steam Audio.

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Only 64-bit Windows is supported")
    endif()
    set(VSA_RID "win-x64")
elseif(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        # libphonon.dylib is built for macOS 11.0+.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
    endif()
    set(VSA_RID "osx")
elseif(UNIX)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "Only x86_64 Linux is supported (Steam Audio ships no arm64 Linux build)")
    endif()
    set(VSA_RID "linux-x64")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

message(STATUS "vsaudio RID: ${VSA_RID}")

# libogg + libvorbis (decoder side only) built as static libraries straight from their source
# lists. Their own CMake projects are not used: libvorbis's find_package(Ogg) does not cooperate
# with an in-tree libogg, and we need none of their install/export machinery.
#
# Outputs:
#   vsa_ogg         static libogg; generated ogg/config_types.h
#   vsa_vorbis      static libvorbis + vorbisfile (no encoder); SYSTEM includes for consumers

set(OGG_DIR "${VSA_THIRD_PARTY_DIR}/libogg")
set(VORBIS_DIR "${VSA_THIRD_PARTY_DIR}/libvorbis")
foreach(_probe "${OGG_DIR}/src/framing.c" "${VORBIS_DIR}/lib/vorbisfile.c")
    if(NOT EXISTS "${_probe}")
        message(FATAL_ERROR "libogg/libvorbis not found (${_probe}); run scripts/fetch-deps first.")
    endif()
endforeach()

# C99 <stdint.h> exists on every compiler we support.
set(INCLUDE_INTTYPES_H 1)
set(INCLUDE_STDINT_H 1)
set(INCLUDE_SYS_TYPES_H 0)
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
set(SIZE64 int64_t)
set(USIZE64 uint64_t)
set(_ogg_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ogg")
configure_file("${OGG_DIR}/include/ogg/config_types.h.in" "${_ogg_generated}/ogg/config_types.h" @ONLY)

add_library(vsa_ogg STATIC "${OGG_DIR}/src/bitwise.c" "${OGG_DIR}/src/framing.c")
target_include_directories(vsa_ogg SYSTEM PUBLIC "${OGG_DIR}/include" "${_ogg_generated}")
vsa_disable_warnings(vsa_ogg)

# Everything in lib/ except the encoder (vorbisenc.c) and the stand-alone tools
# (psytune.c, barkmel.c, tone.c).
set(_vorbis_sources
    analysis.c bitrate.c block.c codebook.c envelope.c floor0.c floor1.c info.c lookup.c lpc.c
    lsp.c mapping0.c mdct.c psy.c registry.c res0.c sharedbook.c smallft.c synthesis.c window.c
    vorbisfile.c)
list(TRANSFORM _vorbis_sources PREPEND "${VORBIS_DIR}/lib/")

add_library(vsa_vorbis STATIC ${_vorbis_sources})
target_include_directories(vsa_vorbis SYSTEM PUBLIC "${VORBIS_DIR}/include")
target_include_directories(vsa_vorbis PRIVATE "${VORBIS_DIR}/lib")
target_link_libraries(vsa_vorbis PUBLIC vsa_ogg)
if(MSVC)
    target_compile_definitions(vsa_vorbis PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
elseif(UNIX)
    target_link_libraries(vsa_vorbis PUBLIC m)
endif()
vsa_disable_warnings(vsa_vorbis)

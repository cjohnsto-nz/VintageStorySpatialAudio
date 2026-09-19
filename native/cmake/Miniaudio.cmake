# miniaudio (single header, fetched into ${VSA_THIRD_PARTY_DIR}/miniaudio) built as a static
# library from one C translation unit. Only device I/O is compiled in: decoding is done by
# libvorbis and our own WAV reader, and miniaudio's high-level engine is not used.
#
# Outputs:
#   vsa_miniaudio   static library; its include directory is a SYSTEM include for consumers

set(MINIAUDIO_DIR "${VSA_THIRD_PARTY_DIR}/miniaudio")
if(NOT EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
    message(FATAL_ERROR "miniaudio not found at ${MINIAUDIO_DIR}; run scripts/fetch-deps first.")
endif()

find_package(Threads REQUIRED)

add_library(vsa_miniaudio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/miniaudio_impl.c")
target_include_directories(vsa_miniaudio SYSTEM PUBLIC "${MINIAUDIO_DIR}")
# These change which parts of miniaudio exist, so every includer must see the same set.
target_compile_definitions(vsa_miniaudio PUBLIC
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
target_link_libraries(vsa_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
    target_link_libraries(vsa_miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    target_link_libraries(vsa_miniaudio PUBLIC m)
endif()
vsa_disable_warnings(vsa_miniaudio)

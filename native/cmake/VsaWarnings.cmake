function(vsa_enable_warnings target)
    if(MSVC)
        # C4324: "structure was padded due to alignment specifier" is informational; we pad on
        # purpose (cache-line separated atomics).
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4324)
        if(VSA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>)
        if(VSA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    vsa_enable_sanitizers(${target})
endfunction()

# Third-party code is compiled as published: no warnings (we do not patch it), and no
# UndefinedBehaviorSanitizer either, since libvorbis's shifts and psy.c's one-past-the-end read
# are not defects anyone here can act on. AddressSanitizer stays on so it still sees the
# allocations, which is how a leak of ours on the far side of a third-party call still shows up.
function(vsa_disable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    vsa_enable_sanitizers(${target} NO_UBSAN)
endfunction()

function(vsa_enable_sanitizers target)
    if(MSVC)
        # Release builds keep a PDB (not shipped): a crash dump from a player's machine can be
        # read. /OPT:REF,ICF restores what /DEBUG would otherwise switch off.
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    endif()
    if(VSA_SANITIZE AND NOT MSVC)
        set(_vsa_sanitizers "address,undefined")
        if("NO_UBSAN" IN_LIST ARGN)
            set(_vsa_sanitizers "address")
        endif()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=${_vsa_sanitizers} -fno-omit-frame-pointer>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:-fsanitize=${_vsa_sanitizers}>)
    endif()
endfunction()

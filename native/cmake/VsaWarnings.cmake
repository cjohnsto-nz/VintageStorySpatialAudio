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

# Third-party code is compiled as published: no warnings (we do not patch it), but still with
# the sanitizers so ASan sees its allocations.
function(vsa_disable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    vsa_enable_sanitizers(${target})
endfunction()

function(vsa_enable_sanitizers target)
    if(VSA_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
    endif()
endfunction()

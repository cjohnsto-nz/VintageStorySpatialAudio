#pragma once

#include "vsaudio.h"

#include <cstdarg>

namespace vsa {

/// Engine-wide log sink. There is at most one engine per process, so the sink is a
/// process-global; Steam Audio's own log callback has no user-data pointer and is
/// routed here as well.
///
/// Not for the real-time render thread (it may allocate and calls user code).
class Log {
public:
    static void set_sink(vsa_log_fn fn, void* user_data) noexcept;
    static void clear_sink() noexcept;

    static void write(vsa_log_level level, const char* message) noexcept;

#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    static void writef(vsa_log_level level, const char* format, ...) noexcept;
};

}  // namespace vsa

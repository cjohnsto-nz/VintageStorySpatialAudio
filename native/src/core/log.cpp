#include "core/log.hpp"

#include <cstdio>
#include <mutex>

namespace vsa {
namespace {

struct Sink {
    vsa_log_fn fn = nullptr;
    void* user_data = nullptr;
};

// Guards the sink so a log call racing with engine destruction cannot call into a
// managed delegate that has already been released.
std::mutex g_sink_mutex;
Sink g_sink;

}  // namespace

void Log::set_sink(vsa_log_fn fn, void* user_data) noexcept {
    std::lock_guard lock(g_sink_mutex);
    g_sink = Sink{fn, user_data};
}

void Log::clear_sink() noexcept {
    std::lock_guard lock(g_sink_mutex);
    g_sink = Sink{};
}

void Log::write(vsa_log_level level, const char* message) noexcept {
    std::lock_guard lock(g_sink_mutex);
    if (g_sink.fn != nullptr && message != nullptr) {
        g_sink.fn(g_sink.user_data, level, message);
    }
}

void Log::writef(vsa_log_level level, const char* format, ...) noexcept {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    write(level, buffer);
}

}  // namespace vsa

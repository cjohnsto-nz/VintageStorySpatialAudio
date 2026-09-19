#include "core/error.hpp"

namespace vsa {
namespace {
thread_local std::string t_last_error;
}

void set_last_error(const std::string& message) noexcept {
    try {
        t_last_error = message;
    } catch (...) {
        // Allocation failed while recording an error; keep whatever we had.
    }
}

void clear_last_error() noexcept { t_last_error.clear(); }

const char* last_error() noexcept { return t_last_error.c_str(); }

}  // namespace vsa

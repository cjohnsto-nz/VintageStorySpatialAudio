#pragma once

#include "vsaudio.h"

#include <stdexcept>
#include <string>

namespace vsa {

/// Exception carrying a vsa_result. Thrown inside the engine, converted to a
/// result code + thread-local message at the C boundary (see api.cpp).
class Error : public std::runtime_error {
public:
    Error(vsa_result code, const std::string& message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] vsa_result code() const noexcept { return code_; }

private:
    vsa_result code_;
};

/// Thread-local "last error" storage backing vsa_get_last_error().
void set_last_error(const std::string& message) noexcept;
void clear_last_error() noexcept;
[[nodiscard]] const char* last_error() noexcept;

}  // namespace vsa

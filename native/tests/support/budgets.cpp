#include "support/budgets.hpp"

#include <cstddef>
#include <cstdlib>

namespace vsa_test {

namespace {

bool read_env() {
#if defined(_MSC_VER)
    // getenv is deprecated under MSVC's secure CRT; getenv_s says the same thing without the
    // warning. A value too long for the buffer fails with len set, which still says the variable
    // is present -- and being present, it is not "0".
    char value[16]{};
    std::size_t len = 0;
    const auto status = getenv_s(&len, value, sizeof value, "VSA_NO_PERF_BUDGETS");
    if (len == 0) {
        return true;
    }
    if (status != 0) {
        return false;
    }
#else
    const char* const value = std::getenv("VSA_NO_PERF_BUDGETS");
    if (value == nullptr) {
        return true;
    }
#endif
    return value[0] == 0 || (value[0] == '0' && value[1] == 0);
}

}  // namespace

bool perf_budgets_enforced() {
    static const bool enforced = read_env();
    return enforced;
}

}  // namespace vsa_test

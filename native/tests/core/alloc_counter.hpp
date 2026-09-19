#pragma once

#include <cstdint>

namespace vsa_test {

/// Counts global operator new calls made on the current thread while the scope is alive. The
/// replacement operators live in alloc_counter.cpp; vsaudio_core_tests links the engine
/// statically, so they see every allocation the engine makes through new.
class AllocationScope {
public:
    AllocationScope() noexcept;
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    [[nodiscard]] uint64_t count() const noexcept;

private:
    uint64_t start_;
};

}  // namespace vsa_test

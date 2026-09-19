#include "core/alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace {

thread_local bool t_counting = false;
thread_local uint64_t t_allocations = 0;

void* allocate(std::size_t size) {
    if (t_counting) {
        ++t_allocations;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    if (t_counting) {
        ++t_allocations;
    }
    const auto align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1) / align * align;
#if defined(_MSC_VER)
    void* p = _aligned_malloc(rounded == 0 ? align : rounded, align);
#else
    void* p = std::aligned_alloc(align, rounded == 0 ? align : rounded);
#endif
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void release_aligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace

namespace vsa_test {

AllocationScope::AllocationScope() noexcept : start_(t_allocations) { t_counting = true; }
AllocationScope::~AllocationScope() { t_counting = false; }
uint64_t AllocationScope::count() const noexcept { return t_allocations - start_; }

}  // namespace vsa_test

// Replacements for every global allocation form.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment) { return allocate_aligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return allocate_aligned(size, alignment); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { release_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release_aligned(p); }

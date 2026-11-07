#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>

namespace ruvia::detail {

// Routes C protocol-library (nghttp2, nghttp3, ngtcp2) allocations through one
// PMR memory_resource. C free() carries no size while memory_resource::deallocate()
// requires it, so each block stores its total size in a max-aligned prefix and
// realloc is emulated by allocate-copy-free. C callbacks must never throw: an
// allocation exception is reported as nullptr and retained so the C++ owner can
// rethrow it after the C call returns.
class http_c_library_memory final {
public:
    explicit http_c_library_memory(std::pmr::memory_resource* resource) noexcept;
    http_c_library_memory(const http_c_library_memory&) = delete;
    http_c_library_memory& operator=(const http_c_library_memory&) = delete;

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    void rethrow_if_failed();
    [[noreturn]] void rethrow_allocation_failure();

    static void* allocate(std::size_t size, void* context) noexcept;
    static void deallocate(void* pointer, void* context) noexcept;
    static void* allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept;
    static void* reallocate(void* pointer, std::size_t size, void* context) noexcept;

private:
    std::pmr::memory_resource* resource_;
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail

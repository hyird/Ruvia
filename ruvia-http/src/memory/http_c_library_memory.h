#pragma once

#include <cstddef>
#include <exception>
#include <limits>

namespace ruvia::detail {

// Routes C protocol-library (nghttp2, nghttp3, ngtcp2) and compression-codec
// (zlib, Brotli, zstd) allocations with size tracking and exception capture.
// C free() carries no size, so each block stores its total size in a
// max-aligned prefix and realloc is emulated by allocate-copy-free. C callbacks
// must never throw: an allocation exception is reported as nullptr and retained
// so the C++ owner can rethrow it after the C call returns.
//
// `native_budget` caps the payload bytes live at once; a request past it is
// refused and remembered by budget_exceeded().
class http_c_library_memory final {
public:
    explicit http_c_library_memory(
        std::size_t native_budget = (std::numeric_limits<std::size_t>::max)(),
        bool throw_on_budget_exceeded = false) noexcept;
    http_c_library_memory(const http_c_library_memory&) = delete;
    http_c_library_memory& operator=(const http_c_library_memory&) = delete;
    void rethrow_if_failed() const;
    [[noreturn]] void rethrow_allocation_failure() const;
    void clear_allocation_failure() noexcept {
        failure_ = nullptr;
        budget_exceeded_ = false;
    }
    [[nodiscard]] bool budget_exceeded() const noexcept {
        return budget_exceeded_;
    }

    static void* allocate(std::size_t size, void* context) noexcept;
    static void deallocate(void* pointer, void* context) noexcept;
    static void* allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept;
    static void* reallocate(void* pointer, std::size_t size, void* context) noexcept;

    static void* allocate_opaque_first(void* context, std::size_t size) noexcept {
        return allocate(size, context);
    }
    static void deallocate_opaque_first(void* context, void* pointer) noexcept {
        deallocate(pointer, context);
    }

private:
    std::size_t native_budget_;
    std::size_t live_bytes_{0};
    mutable std::exception_ptr failure_;
    bool budget_exceeded_{false};
    bool throw_on_budget_exceeded_{false};
};

template <typename table_type>
[[nodiscard]] constexpr table_type make_c_library_mem_table(http_c_library_memory& memory) noexcept {
    return table_type{
        &memory,
        &http_c_library_memory::allocate,
        &http_c_library_memory::deallocate,
        &http_c_library_memory::allocate_zeroed,
        &http_c_library_memory::reallocate,
    };
}

}  // namespace ruvia::detail

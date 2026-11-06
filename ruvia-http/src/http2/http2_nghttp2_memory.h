#pragma once

#include <nghttp2/nghttp2.h>

#include <exception>
#include <memory_resource>

namespace ruvia::detail {

// Routes every nghttp2 allocation through one PMR resource. nghttp2_free()
// carries no size while memory_resource::deallocate() needs it, so each block
// stores its total size in a max-aligned prefix and realloc is emulated by
// allocate-copy-free. nghttp2 must never see an exception: a throwing resource
// is reported to nghttp2 as NULL (NGHTTP2_ERR_NOMEM) and the first exception is
// retained so the owner can rethrow it after the C call returns.
class http2_nghttp2_memory final {
public:
    explicit http2_nghttp2_memory(std::pmr::memory_resource* resource) noexcept;
    http2_nghttp2_memory(const http2_nghttp2_memory&) = delete;
    http2_nghttp2_memory& operator=(const http2_nghttp2_memory&) = delete;

    // The returned table points at this object, which must outlive every
    // nghttp2 object created from it.
    [[nodiscard]] nghttp2_mem* table() noexcept {
        return &table_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    // Rethrows the allocation failure recorded since the last call, or throws
    // std::bad_alloc when nghttp2 reported NOMEM without one.
    [[noreturn]] void rethrow_allocation_failure();

private:
    static void* allocate(std::size_t size, void* context) noexcept;
    static void deallocate(void* pointer, void* context) noexcept;
    static void* allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept;
    static void* reallocate(void* pointer, std::size_t size, void* context) noexcept;

    std::pmr::memory_resource* resource_;
    nghttp2_mem table_;
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail

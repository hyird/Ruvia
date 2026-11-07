#pragma once

#include <nghttp2/nghttp2.h>

#include <memory_resource>

#include "util/http_c_library_memory.h"

namespace ruvia::detail {

// Routes every nghttp2 allocation through one PMR resource via http_c_library_memory.
class http2_nghttp2_memory final {
public:
    explicit http2_nghttp2_memory(std::pmr::memory_resource* resource) noexcept
        : memory_(resource),
          table_{.mem_user_data = &memory_,
              .malloc = &http_c_library_memory::allocate,
              .free = &http_c_library_memory::deallocate,
              .calloc = &http_c_library_memory::allocate_zeroed,
              .realloc = &http_c_library_memory::reallocate} {}
    http2_nghttp2_memory(const http2_nghttp2_memory&) = delete;
    http2_nghttp2_memory& operator=(const http2_nghttp2_memory&) = delete;

    // The returned table points at this object, which must outlive every
    // nghttp2 object created from it.
    [[nodiscard]] nghttp2_mem* table() noexcept {
        return &table_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

    // Rethrows the allocation failure recorded since the last call, or throws
    // std::bad_alloc when nghttp2 reported NOMEM without one.
    [[noreturn]] void rethrow_allocation_failure() {
        memory_.rethrow_allocation_failure();
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem table_;
};

}  // namespace ruvia::detail

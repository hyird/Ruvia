#pragma once

#include <cstddef>
#include <limits>

namespace ruvia {

namespace detail {
class http_c_library_memory;
}

// Native codec working-memory budget shared by one request's encoders and
// decoders. Not thread-safe; its address and callback context must remain valid
// until every codec using it has been destroyed. No pool or arena is involved.
// The budget counts live native payload bytes (not allocator bookkeeping).
struct http_codec_allocation_callbacks final {
    void* context_{nullptr};
    void* (*allocate_)(std::size_t bytes, std::size_t alignment, void* context){nullptr};
    void (*deallocate_)(void* pointer, std::size_t bytes, std::size_t alignment, void* context) noexcept {nullptr};
};

class http_codec_memory final {
public:
    // Both callbacks must be supplied together; supplying neither selects
    // operator new / delete. Allocation may throw any exception, captured at the C
    // boundary and rethrown unchanged. Returning nullptr means std::bad_alloc.
    explicit http_codec_memory(std::size_t max_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_allocation_callbacks callbacks = {});
    http_codec_memory(const http_codec_memory&) = delete;
    http_codec_memory& operator=(const http_codec_memory&) = delete;

    [[nodiscard]] std::size_t live_bytes() const noexcept {
        return live_bytes_;
    }
    [[nodiscard]] std::size_t max_bytes() const noexcept {
        return max_bytes_;
    }

private:
    friend class detail::http_c_library_memory;
    [[nodiscard]] void* allocate(std::size_t payload_bytes, std::size_t total_bytes, std::size_t alignment);
    void deallocate(void* pointer, std::size_t payload_bytes, std::size_t total_bytes, std::size_t alignment) noexcept;

    std::size_t max_bytes_;
    std::size_t live_bytes_{0};
    http_codec_allocation_callbacks callbacks_;
};

}  // namespace ruvia

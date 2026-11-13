#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

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

namespace ruvia::detail {

// Ensures capacity for `required` elements before a mutation that must not
// reallocate. Growth at least doubles the current capacity because an exact
// reserve(n) (libc++) would turn a sequence of small appends into quadratic copying.
template <typename container_type>
void reserve_geometric(container_type& container, std::size_t required) {
    const auto capacity = container.capacity();
    if (required <= capacity) {
        return;
    }
    const auto max_size = container.max_size();
    if (required > max_size) {
        throw std::length_error("container capacity overflow");
    }
    const auto doubled = capacity > max_size / 2 ? max_size : capacity * 2;
    container.reserve((std::max)(required, doubled));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Address intervals compare as unsigned integers: views into unrelated buffers
// are unrelated pointers, and subtraction avoids forming an overflowing end.
[[nodiscard]] inline bool http_text_overlaps(std::string_view left, std::string_view right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_address = reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_address = reinterpret_cast<std::uintptr_t>(right.data());
    return left_address >= right_address ? left_address - right_address < right.size()
                                         : right_address - left_address < left.size();
}

// Whether `text` views bytes `storage` owns, its terminator included: growing or
// rewriting `storage` would destroy that input, so the caller owns a copy first.
[[nodiscard]] inline bool http_text_aliases_string(std::string_view text, const std::string& storage) noexcept {
    return http_text_overlaps(text, std::string_view(storage.data(), storage.capacity() + 1));
}

}  // namespace ruvia::detail

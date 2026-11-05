#pragma once

#include <cstddef>
#include <exception>
#include <limits>
#include <memory_resource>

// Codec free callbacks need not retain the allocating resource themselves.
// The implementation stamps each block with its originating PMR resource and
// total allocation size so codec state follows the same ownership boundary as
// protocol output.

namespace ruvia::detail {

// Codec stream types own this context and pass it through the C allocator
// callback. Allocation exceptions are retained at the ABI boundary and rethrown
// as soon as the codec returns to C++.
//
// `native_budget` caps the bytes of native state live at once. A request past
// it is refused like an allocation failure, without touching the resource, and
// is remembered by budget_exceeded() so the owner can report the bound rather
// than an out-of-memory codec.
class pmr_codec_allocation_context final {
public:
    explicit pmr_codec_allocation_context(std::pmr::memory_resource* resource,
        std::size_t native_budget = (std::numeric_limits<std::size_t>::max)()) noexcept
        : resource_(resource),
          native_budget_(native_budget) {}

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    void capture_current_exception() noexcept;
    void rethrow_allocation_failure() const;
    // A reusable codec forgets a failure that was already reported once its
    // native state has been reset for the next unit of work.
    void clear_allocation_failure() noexcept {
        failure_ = nullptr;
        budget_exceeded_ = false;
    }
    [[nodiscard]] bool budget_exceeded() const noexcept {
        return budget_exceeded_;
    }

    // Accounting used only by the allocator callbacks.
    [[nodiscard]] bool reserve_native_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > native_budget_ - live_bytes_) {
            budget_exceeded_ = true;
            return false;
        }
        live_bytes_ += bytes_value;
        return true;
    }
    void release_native_bytes(std::size_t bytes_value) noexcept {
        live_bytes_ -= bytes_value;
    }

private:
    std::pmr::memory_resource* resource_;
    std::size_t native_budget_;
    std::size_t live_bytes_{0};
    std::exception_ptr failure_;
    bool budget_exceeded_{false};
};

[[nodiscard]] void* pmr_codec_allocate(void* opaque, std::size_t bytes_value) noexcept;
void pmr_codec_free(void* opaque, void* address) noexcept;

}  // namespace ruvia::detail

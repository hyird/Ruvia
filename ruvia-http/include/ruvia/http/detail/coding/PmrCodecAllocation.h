#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>

// Codec free callbacks need not retain the allocating resource themselves.
// The implementation stamps each block with its originating PMR resource and
// total allocation size so codec state follows the same ownership boundary as
// protocol output.

namespace ruvia::detail {

// Whole-buffer codec calls pass this stack-owned context through the C allocator
// callback. Allocation exceptions are retained at the ABI boundary and rethrown
// as soon as the codec returns to C++.
class pmr_codec_allocation_context final {
public:
    explicit pmr_codec_allocation_context(std::pmr::memory_resource* resource) noexcept
        : resource_(resource) {}

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    void capture_current_exception() noexcept;
    void rethrow_allocation_failure() const;

private:
    std::pmr::memory_resource* resource_;
    std::exception_ptr failure_;
};

[[nodiscard]] void* pmrCodecAllocate(void* opaque, std::size_t bytes) noexcept;
[[nodiscard]] void* pmr_codec_allocate_with_exception(void* opaque, std::size_t bytes) noexcept;
void pmrCodecFree(void* opaque, void* address) noexcept;

}  // namespace ruvia::detail

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

constexpr std::size_t kResponseHeadStackBytes = 512;
constexpr std::size_t kResponseHeadRetainedHeapBytes = 4 * 1024;

class ResponseHeadBuffer final {
public:
    explicit ResponseHeadBuffer(std::pmr::polymorphic_allocator<char> allocator) : heap_(allocator) {}

    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void appendUnsigned(std::uint64_t value);
    void reserveAdditional(std::size_t size);
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool canAppendOnStack(std::size_t size) const noexcept;

    // Bulk fast path: returns a raw cursor when `bound` bytes are guaranteed to
    // fit in the stack buffer, so callers can emit without per-append checks.
    [[nodiscard]] char* stackCursor(std::size_t bound) noexcept {
        if (overflowed_ || bound > stack_.size() - used_) {
            return nullptr;
        }
        return stack_.data() + used_;
    }

    void commitStack(const char* end) noexcept {
        used_ = static_cast<std::size_t>(end - stack_.data());
    }

private:
    void spillToHeap(std::size_t minCapacity);

    std::array<char, kResponseHeadStackBytes> stack_{};
    std::pmr::string heap_;
    std::size_t used_{0};
    bool overflowed_{false};
};

}  // namespace ruvia::detail

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

namespace ruvia::detail {

constexpr std::size_t kResponseHeadStackBytes = 512;
constexpr std::size_t kResponseHeadRetainedHeapBytes = 4 * 1024;

class ResponseHeadBuffer final {
    struct StackState final {
        std::size_t used{0};
    };

    struct HeapState final {};

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
        auto* const stackState = std::get_if<StackState>(&state_);
        if (stackState == nullptr ||
            bound > stack_.size() - stackState->used) {
            return nullptr;
        }
        return stack_.data() + stackState->used;
    }

    void commitStack(const char* end) noexcept {
        std::get<StackState>(state_).used =
            static_cast<std::size_t>(end - stack_.data());
    }

private:
    void spillToHeap(std::size_t minCapacity);

    std::array<char, kResponseHeadStackBytes> stack_{};
    std::pmr::string heap_;
    // heap_ remains allocated while the scratch object is pooled, but this
    // discriminant is the sole authority for which storage contains the head.
    // It prevents impossible combinations such as heap-active with a live
    // stack length from leaking across reset/retry paths.
    std::variant<StackState, HeapState> state_{StackState{}};
};

}  // namespace ruvia::detail

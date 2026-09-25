#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace ruvia {

inline constexpr std::size_t kResponseHeadStackBytes = 512;
inline constexpr std::size_t kResponseHeadRetainedHeapBytes = std::size_t{4} * 1024;

// Reusable scratch storage for serialized HTTP response heads. Keeps small heads
// inline and retains only bounded heap capacity across requests.
class HttpResponseHeadBuffer final {
    struct StackState final {
        std::size_t used{0};
    };
    struct HeapState final {};

public:
    explicit HttpResponseHeadBuffer(std::pmr::polymorphic_allocator<char> allocator)
        : heap_(allocator) {}

    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void appendUnsigned(std::uint64_t value);
    void reserveAdditional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool canAppendOnStack(std::size_t size) const noexcept;

    template <typename Writer>
        requires std::is_nothrow_invocable_r_v<void, Writer&, char*>
    void appendGenerated(std::size_t size, Writer&& writer) {
        if (char* cursor = stackCursor(size); cursor != nullptr) {
            writer(cursor);
            commitStack(cursor + size);
            return;
        }
        reserveAdditional(size);
        const auto previousSize = heap_.size();
        heap_.resize_and_overwrite(previousSize + size, [&](char* bytes, std::size_t) noexcept {
            writer(bytes + previousSize);
            return previousSize + size;
        });
    }

    [[nodiscard]] char* stackCursor(std::size_t bound) & noexcept {
        auto* const stackState = std::get_if<StackState>(&state_);
        if (stackState == nullptr || bound > stack_.size() - stackState->used) {
            return nullptr;
        }
        return stack_.data() + stackState->used;
    }
    [[nodiscard]] char* stackCursor(std::size_t) && = delete;

    void commitStack(const char* end) noexcept {
        std::get<StackState>(state_).used = static_cast<std::size_t>(end - stack_.data());
    }

private:
    void spillToHeap(std::size_t minCapacity);
    std::array<char, kResponseHeadStackBytes> stack_{};
    std::pmr::string heap_;
    std::variant<StackState, HeapState> state_{StackState{}};
};

namespace detail {
using ResponseHeadBuffer = HttpResponseHeadBuffer;
inline constexpr auto kResponseHeadStackBytes = ::ruvia::kResponseHeadStackBytes;
inline constexpr auto kResponseHeadRetainedHeapBytes = ::ruvia::kResponseHeadRetainedHeapBytes;
}  // namespace detail

}  // namespace ruvia

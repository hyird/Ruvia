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

inline constexpr std::size_t response_head_stack_bytes = 512;
inline constexpr std::size_t response_head_retained_heap_bytes = std::size_t{4} * 1024;

// Reusable scratch storage for serialized HTTP response heads. Keeps small heads
// inline and retains only bounded heap capacity across requests.
class http_response_head_buffer final {
    struct stack_state_type final {
        std::size_t used_{0};
    };
    struct heap_state_type final {};

public:
    explicit http_response_head_buffer(std::pmr::polymorphic_allocator<char> allocator)
        : heap_(allocator) {}

    void reset() noexcept;
    void append(std::string_view value);
    void append(char value);
    void append_unsigned(std::uint64_t value);
    void reserve_additional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool can_append_on_stack(std::size_t size) const noexcept;

    template <typename writer_type>
        requires std::is_nothrow_invocable_r_v<void, writer_type&, char*>
    void append_generated(std::size_t size, writer_type&& writer) {
        if (char* cursor_value = stack_cursor(size); cursor_value != nullptr) {
            writer(cursor_value);
            commit_stack(cursor_value + size);
            return;
        }
        reserve_additional(size);
        const auto previous_size = heap_.size();
        heap_.resize(previous_size + size);
        writer(heap_.data() + previous_size);
    }

    [[nodiscard]] char* stack_cursor(std::size_t bound) & noexcept {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr || bound > stack_.size() - stack_state->used_) {
            return nullptr;
        }
        return stack_.data() + stack_state->used_;
    }
    [[nodiscard]] char* stack_cursor(std::size_t) && = delete;

    void commit_stack(const char* end) noexcept {
        std::get<stack_state_type>(state_).used_ = static_cast<std::size_t>(end - stack_.data());
    }

private:
    void spill_to_heap(std::size_t min_capacity);
    std::array<char, response_head_stack_bytes> stack_{};
    std::pmr::string heap_;
    std::variant<stack_state_type, heap_state_type> state_{stack_state_type{}};
};

namespace detail {
using response_head_buffer_type = http_response_head_buffer;
inline constexpr auto response_head_stack_bytes = ::ruvia::response_head_stack_bytes;
inline constexpr auto response_head_retained_heap_bytes = ::ruvia::response_head_retained_heap_bytes;
}  // namespace detail

}  // namespace ruvia

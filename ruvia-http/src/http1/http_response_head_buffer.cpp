#include "server/http_response_head_buffer.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <variant>

#include "util/pmr_string.h"

namespace ruvia {

void http_response_head_buffer::reset() noexcept {
    // Same retain-small-else-release policy as every other pooled scratch buffer.
    detail::clear_pmr_string_retaining_small(heap_, response_head_retained_heap_bytes);
    state_.emplace<stack_state_type>();
}

void http_response_head_buffer::spill_to_heap(std::size_t min_capacity) {
    const auto* const stack_state = std::get_if<stack_state_type>(&state_);
    if (stack_state == nullptr) {
        return;
    }

    heap_.reserve(std::max(stack_.size() * 2, min_capacity));
    heap_.assign(stack_.data(), stack_state->used_);
    state_.emplace<heap_state_type>();
}

void http_response_head_buffer::append(std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (value.size() <= stack_.size() - stack_state->used_) {
            std::memcpy(stack_.data() + stack_state->used_, value.data(), value.size());
            stack_state->used_ += value.size();
            return;
        }
        if (value.size() > heap_.max_size() - stack_state->used_) {
            throw std::length_error("HTTP response head is too large");
        }
        spill_to_heap(stack_state->used_ + value.size());
    }
    heap_.append(value);
}

void http_response_head_buffer::append(char value) {
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (stack_state->used_ < stack_.size()) {
            stack_[stack_state->used_++] = value;
            return;
        }
        spill_to_heap(stack_state->used_ + 1);
    }
    heap_.push_back(value);
}

void http_response_head_buffer::append_unsigned(std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        append(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
}

void http_response_head_buffer::reserve_additional(std::size_t size) {
    if (std::holds_alternative<heap_state_type>(state_)) {
        if (size > heap_.max_size() - heap_.size()) {
            throw std::length_error("HTTP response head is too large");
        }
        heap_.reserve(heap_.size() + size);
        return;
    }
    const auto used = std::get<stack_state_type>(state_).used_;
    if (size > heap_.max_size() - used) {
        throw std::length_error("HTTP response head is too large");
    }
    spill_to_heap(used + size);
}

std::string_view http_response_head_buffer::view() const& noexcept {
    if (const auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        return std::string_view(stack_.data(), stack_state->used_);
    }
    return std::string_view(heap_);
}

bool http_response_head_buffer::can_append_on_stack(std::size_t size) const noexcept {
    const auto* const stack_state = std::get_if<stack_state_type>(&state_);
    return stack_state != nullptr && size <= stack_.size() - stack_state->used_;
}

}  // namespace ruvia

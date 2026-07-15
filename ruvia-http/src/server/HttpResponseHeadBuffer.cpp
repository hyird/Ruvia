#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"

#include "ruvia/http/detail/PmrString.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace ruvia::detail {

void ResponseHeadBuffer::reset() noexcept {
    // Same retain-small-else-release policy as every other pooled scratch buffer.
    clearPmrStringRetainingSmall(heap_, kResponseHeadRetainedHeapBytes);
    state_.emplace<StackState>();
}

void ResponseHeadBuffer::spillToHeap(std::size_t minCapacity) {
    const auto* const stackState = std::get_if<StackState>(&state_);
    if (stackState == nullptr) {
        return;
    }

    heap_.reserve(std::max(stack_.size() * 2, minCapacity));
    heap_.assign(stack_.data(), stackState->used);
    state_.emplace<HeapState>();
}

void ResponseHeadBuffer::append(std::string_view value) {
    if (auto* const stackState = std::get_if<StackState>(&state_)) {
        if (value.size() <= stack_.size() - stackState->used) {
            std::memcpy(
                stack_.data() + stackState->used, value.data(), value.size());
            stackState->used += value.size();
            return;
        }
        if (value.size() > heap_.max_size() - stackState->used) {
            throw std::length_error("HTTP response head is too large");
        }
        spillToHeap(stackState->used + value.size());
    }
    heap_.append(value);
}

void ResponseHeadBuffer::append(char value) {
    if (auto* const stackState = std::get_if<StackState>(&state_)) {
        if (stackState->used < stack_.size()) {
            stack_[stackState->used++] = value;
            return;
        }
        spillToHeap(stackState->used + 1);
    }
    heap_.push_back(value);
}

void ResponseHeadBuffer::appendUnsigned(std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        append(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
}

void ResponseHeadBuffer::reserveAdditional(std::size_t size) {
    if (std::holds_alternative<HeapState>(state_)) {
        if (size > heap_.max_size() - heap_.size()) {
            throw std::length_error("HTTP response head is too large");
        }
        heap_.reserve(heap_.size() + size);
        return;
    }
    const auto used = std::get<StackState>(state_).used;
    if (size > heap_.max_size() - used) {
        throw std::length_error("HTTP response head is too large");
    }
    spillToHeap(used + size);
}

std::string_view ResponseHeadBuffer::view() const noexcept {
    if (const auto* const stackState = std::get_if<StackState>(&state_)) {
        return std::string_view(stack_.data(), stackState->used);
    }
    return std::string_view(heap_);
}

bool ResponseHeadBuffer::canAppendOnStack(std::size_t size) const noexcept {
    const auto* const stackState = std::get_if<StackState>(&state_);
    return stackState != nullptr &&
        size <= stack_.size() - stackState->used;
}

}  // namespace ruvia::detail

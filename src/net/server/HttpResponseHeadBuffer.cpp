#include "HttpResponseHeadBuffer.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace ruvia::detail {

void ResponseHeadBuffer::reset() noexcept {
    if (heap_.capacity() > kResponseHeadRetainedHeapBytes) {
        std::pmr::string replacement(heap_.get_allocator());
        heap_.swap(replacement);
    } else {
        heap_.clear();
    }
    used_ = 0;
    overflowed_ = false;
}

void ResponseHeadBuffer::spillToHeap(std::size_t minCapacity) {
    if (overflowed_) {
        return;
    }

    heap_.reserve(std::max(stack_.size() * 2, minCapacity));
    heap_.assign(stack_.data(), used_);
    overflowed_ = true;
}

void ResponseHeadBuffer::append(std::string_view value) {
    if (!overflowed_ && value.size() <= stack_.size() - used_) {
        std::memcpy(stack_.data() + used_, value.data(), value.size());
        used_ += value.size();
        return;
    }
    if (!overflowed_) {
        spillToHeap(used_ + value.size());
    }
    heap_.append(value);
}

void ResponseHeadBuffer::append(char value) {
    if (!overflowed_ && used_ < stack_.size()) {
        stack_[used_++] = value;
        return;
    }
    if (!overflowed_) {
        spillToHeap(used_ + 1);
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
    if (overflowed_) {
        heap_.reserve(heap_.size() + size);
        return;
    }
    spillToHeap(used_ + size);
}

std::string_view ResponseHeadBuffer::view() const noexcept {
    return overflowed_ ? std::string_view(heap_) : std::string_view(stack_.data(), used_);
}

bool ResponseHeadBuffer::canAppendOnStack(std::size_t size) const noexcept {
    return !overflowed_ && size <= stack_.size() - used_;
}

}  // namespace ruvia::detail

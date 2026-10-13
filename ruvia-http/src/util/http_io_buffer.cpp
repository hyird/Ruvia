#include "ruvia/http/http_io_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "util/http_text_overlap.h"

namespace ruvia {
namespace {

// Keeps every offset and size representable as a pointer difference.
constexpr std::size_t max_io_buffer_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());

}  // namespace

http_io_buffer::http_io_buffer(http_io_buffer&& other) noexcept
    : allocator_(other.allocator_),
      storage_(std::exchange(other.storage_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)),
      begin_(std::exchange(other.begin_, 0)),
      end_(std::exchange(other.end_, 0)) {}

http_io_buffer::~http_io_buffer() {
    release();
}

std::span<char> http_io_buffer::prepare(std::size_t bytes) {
    if (capacity_ - end_ < bytes) {
        make_room(bytes);
    }
    return std::span<char>(storage_ + end_, bytes);
}

void http_io_buffer::commit(std::size_t bytes) {
    if (bytes > capacity_ - end_) {
        throw std::length_error("http_io_buffer commit exceeds the prepared space");
    }
    end_ += bytes;
}

void http_io_buffer::append(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (capacity_ - end_ >= bytes.size()) {
        // The source may be readable bytes of this buffer, which never
        // overlap the writable space but may share its storage.
        std::memmove(storage_ + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
        return;
    }
    const bool aliases = detail::http_text_overlaps(bytes, std::string_view(storage_, capacity_));
    if (!aliases) {
        make_room(bytes.size());
        std::memcpy(storage_ + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
        return;
    }
    // Copy an aliasing source into new storage before the old one is freed.
    reallocate(room_capacity(bytes.size()), bytes);
}

void http_io_buffer::consume(std::size_t bytes) noexcept {
    begin_ += std::min(bytes, size());
    if (begin_ == end_) {
        clear();
    }
}

void http_io_buffer::reserve(std::size_t bytes) {
    if (bytes > max_io_buffer_bytes) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    if (bytes > capacity_) {
        reallocate(bytes);
    }
}

void http_io_buffer::shrink_to(std::size_t bytes) {
    const auto target = std::max(size(), bytes);
    if (capacity_ > target) {
        reallocate(target);
    }
}

void http_io_buffer::clear_retaining_small(std::size_t retained_bytes) noexcept {
    clear();
    if (capacity_ > retained_bytes) {
        release();
    }
}

std::size_t http_io_buffer::room_capacity(std::size_t bytes) const {
    if (bytes > max_io_buffer_bytes - size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    // Charge compaction to the consumed prefix: moving the live bytes must
    // recover at least as many bytes. Otherwise grow once for future writes.
    if (capacity_ - size() >= bytes && begin_ >= size()) {
        return capacity_;
    }
    const auto doubled = capacity_ > max_io_buffer_bytes / 2 ? max_io_buffer_bytes : capacity_ * 2;
    return std::max(size() + bytes, doubled);
}

void http_io_buffer::make_room(std::size_t bytes) {
    const auto capacity = room_capacity(bytes);
    if (capacity > capacity_) {
        reallocate(capacity);
        return;
    }
    // Only the readable bytes move, and only when the dead prefix is needed.
    const auto live = size();
    std::memmove(storage_, storage_ + begin_, live);
    begin_ = 0;
    end_ = live;
}

void http_io_buffer::reallocate(std::size_t capacity, std::string_view tail) {
    const auto live = size();
    char* storage = capacity == 0 ? nullptr : allocator_.allocate(capacity);
    if (live != 0) {
        std::memcpy(storage, storage_ + begin_, live);
    }
    if (!tail.empty()) {
        std::memcpy(storage + live, tail.data(), tail.size());
    }
    release();
    storage_ = storage;
    capacity_ = capacity;
    begin_ = 0;
    end_ = live + tail.size();
}

void http_io_buffer::release() noexcept {
    if (storage_ != nullptr) {
        allocator_.deallocate(storage_, capacity_);
    }
    storage_ = nullptr;
    capacity_ = 0;
    begin_ = 0;
    end_ = 0;
}

}  // namespace ruvia

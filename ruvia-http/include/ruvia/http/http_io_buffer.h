#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/http/http_pmr_string.h"

namespace ruvia {

// PMR byte buffer for transport input: a readable window followed by writable
// space. A read fills the span prepare() returns and commit() publishes the
// bytes it wrote. Unlike std::pmr::string, growing for a read never
// initializes the bytes the read is about to overwrite, and uninitialized
// bytes are never part of the readable window.
//
// consume() only advances the window, so views of readable bytes stay valid
// until the next prepare(), append(), reserve(), shrink_to() or
// clear_retaining_small(). The window moves to the front only when prepare()
// or append() needs the room.
class http_io_buffer final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<char>;

    http_io_buffer() noexcept = default;
    explicit http_io_buffer(allocator_type allocator) noexcept
        : allocator_(allocator) {}
    http_io_buffer(const http_io_buffer&) = delete;
    http_io_buffer& operator=(const http_io_buffer&) = delete;
    http_io_buffer(http_io_buffer&& other) noexcept;
    http_io_buffer& operator=(http_io_buffer&&) = delete;
    ~http_io_buffer();

    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return allocator_;
    }

    // Readable bytes.
    [[nodiscard]] char* data() noexcept {
        return storage_ + begin_;
    }
    [[nodiscard]] const char* data() const noexcept {
        return storage_ + begin_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return end_ - begin_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return begin_ == end_;
    }
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(data(), size());
    }
    // Bytes the storage holds without reallocating, readable bytes included.
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    // Exactly `bytes` uninitialized writable bytes after the readable ones.
    // Moves the readable bytes to the front when that makes the room;
    // otherwise grows the storage geometrically. Throws std::length_error when
    // the size would exceed the addressable range.
    [[nodiscard]] std::span<char> prepare(std::size_t bytes);
    // Appends the first `bytes` bytes of the writable space, which the caller
    // has written. Throws std::length_error past the storage.
    void commit(std::size_t bytes);
    // Appends a copy of `bytes`, which may alias this buffer.
    void append(std::string_view bytes);
    // Drops the first min(bytes, size()) readable bytes without moving any.
    void consume(std::size_t bytes) noexcept;

    // Grows the storage to at least `bytes` (exactly `bytes` when it grows).
    void reserve(std::size_t bytes);
    // Releases storage beyond max(size(), bytes).
    void shrink_to(std::size_t bytes);
    // Drops every readable byte and keeps the storage.
    void clear() noexcept {
        begin_ = 0;
        end_ = 0;
    }
    // Drops every readable byte and returns storage larger than
    // `retained_bytes` to the allocator.
    void clear_retaining_small(std::size_t retained_bytes = http_retained_pmr_string_bytes) noexcept;

private:
    [[nodiscard]] std::size_t grown_capacity(std::size_t bytes) const;
    void make_room(std::size_t bytes);
    void reallocate(std::size_t capacity, std::string_view tail = {});
    void release() noexcept;

    allocator_type allocator_{};
    char* storage_{nullptr};
    std::size_t capacity_{0};
    std::size_t begin_{0};
    std::size_t end_{0};
};

}  // namespace ruvia

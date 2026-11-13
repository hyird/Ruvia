#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// Retention table: the most storage a reusable ruvia-http buffer keeps once it
// is drained. Anything larger goes back to the allocator, so one large message
// or head never stays pinned to an idle connection or worker, while buffers
// within the bound are reused without reallocating.

// Worker-local temporary strings and buffers (heads, client write buffers).
inline constexpr std::size_t http_retained_string_bytes = 4096;

// Each WebSocket connection buffer: transport input, pending output,
// compressed outbound payload and decompressed inbound message.
inline constexpr std::size_t websocket_retained_buffer_bytes = std::size_t{16} * 1024;

// Transport read that waits for the next frame on a server WebSocket
// connection whose recent frames were small. It bounds the read storage an
// idle connection holds; a larger frame grows the buffer on demand.
inline constexpr std::size_t websocket_idle_read_bytes = 512;

// Clear a temporary buffer, returning unusually large storage to the allocator
// while retaining small allocations for repeated worker-local operations.
inline void http_clear_string_retaining_small(
    std::string& target, std::size_t retained_bytes = http_retained_string_bytes) noexcept {
    target.clear();
    if (target.capacity() <= retained_bytes) {
        return;
    }
    std::string empty;
    target.swap(empty);
}

// Byte buffer for transport input: a readable window followed by writable
// space. A read fills the span prepare() or prepare_spare() returns; commit()
// publishes the bytes it wrote. Unlike std::string, growing for a read never
// initializes the bytes the read is about to overwrite, and uninitialized
// bytes are never part of the readable window.
// Every storage borrow requires an lvalue owner. Keep the storage alive until
// its readable view or writable span has been consumed, including pending I/O.
//
// consume() only advances the window, so views of readable bytes stay valid
// until the next prepare(), prepare_spare(), append(), reserve(), shrink_to()
// or clear_retaining_small(). Compaction only moves live bytes when the
// consumed prefix covers them; existing writable tail space is used first.
class http_io_buffer final {
public:
    http_io_buffer() noexcept = default;
    http_io_buffer(const http_io_buffer&) = delete;
    http_io_buffer& operator=(const http_io_buffer&) = delete;
    http_io_buffer(http_io_buffer&& other) noexcept;
    http_io_buffer& operator=(http_io_buffer&&) = delete;
    ~http_io_buffer();

    // Readable bytes.
    [[nodiscard]] char* data() & noexcept RUVIA_LIFETIMEBOUND {
        return storage_ + begin_;
    }
    [[nodiscard]] const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return storage_ + begin_;
    }
    char* data() && = delete;
    const char* data() const&& = delete;
    [[nodiscard]] std::size_t size() const noexcept {
        return end_ - begin_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return begin_ == end_;
    }
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(data(), size());
    }
    std::string_view view() const&& = delete;
    // Bytes the storage holds without reallocating, readable bytes included.
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    // Exactly `bytes` uninitialized writable bytes after the readable ones.
    // Compacts when the consumed prefix covers the live bytes and makes room;
    // otherwise grows geometrically to amortize small read/write cycles.
    // Throws std::length_error when the size exceeds the addressable range.
    [[nodiscard]] std::span<char> prepare(std::size_t bytes) & RUVIA_LIFETIMEBOUND;
    std::span<char> prepare(std::size_t) && = delete;
    // Returns the current writable tail without growing. Only when that tail
    // is empty and the consumed prefix covers the live bytes is it reclaimed.
    // The result may be empty even when capacity() > size(); a reader must
    // prepare() more room before issuing I/O if no spare space is returned.
    [[nodiscard]] std::span<char> prepare_spare() & noexcept RUVIA_LIFETIMEBOUND;
    std::span<char> prepare_spare() && = delete;
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
    void clear_retaining_small(std::size_t retained_bytes = http_retained_string_bytes) noexcept;

private:
    [[nodiscard]] std::size_t room_capacity(std::size_t bytes) const;
    void make_room(std::size_t bytes);
    void move_window_to_front() noexcept;
    void reallocate(std::size_t capacity, std::string_view tail = {});
    void release() noexcept;

    char* storage_{nullptr};
    std::size_t capacity_{0};
    std::size_t begin_{0};
    std::size_t end_{0};
};

}  // namespace ruvia

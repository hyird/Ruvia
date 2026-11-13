#pragma once

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia::detail {

// A C-string entry point is kept for literal-friendly APIs, but nullptr must
// not reach std::string_view(const char*)'s traits::length call. Normalizing it
// to the same empty view as the default wrapper keeps these noexcept borrowed
// values defined; consuming validators still decide whether empty is valid.
[[nodiscard]] constexpr std::string_view http_borrowed_c_string_view(const char* value) noexcept {
    return value == nullptr ? std::string_view{} : std::string_view(value);
}

// Zero-copy protocol values frequently retain views into caller-owned character
// storage. Use this shared predicate on deleted overloads so a temporary
// std::string cannot silently satisfy a
// std::string_view parameter and leave the returned protocol value dangling.
template <typename t_type>
inline constexpr bool is_http_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_http_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept http_temporary_owning_char_string =
    is_http_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

// std::span<const element_type> parameters bind every contiguous range,
// including temporary std::string, std::vector, and std::array values. Use this
// shared predicate on deleted overloads of span entry points whose results
// borrow the input; borrowed ranges (span, string_view) and lvalues still bind.
template <typename range_type, typename element_type>
concept http_temporary_owning_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>;

}  // namespace ruvia::detail

#ifndef RUVIA_LIFETIMEBOUND
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(gnu::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[gnu::lifetimebound]]
#else
#define RUVIA_LIFETIMEBOUND
#endif
#else
#define RUVIA_LIFETIMEBOUND
#endif
#endif

namespace ruvia {

// Zero-copy text held by a configuration or message value that outlives the
// call which set it -- cookie attributes, SSE fields, security header policies,
// Redis SCAN patterns, outbound request lines.
//
// It is a std::string_view that refuses to be built from an owning temporary.
// Those values are commonly stored and read back later, so binding one to a
// std::string rvalue would leave the stored view dangling at the point of use
// rather than at the point of the mistake. String literals, string_view values,
// and owning-string lvalues all remain valid inputs.
class borrowed_text final {
public:
    constexpr borrowed_text() noexcept = default;

    constexpr borrowed_text(std::string_view value) noexcept
        : value_(value) {}

    constexpr borrowed_text(const char* value) noexcept
        : value_(detail::http_borrowed_c_string_view(value)) {}

    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text(const std::basic_string<char, traits_type, allocator_type>& value) noexcept
        : value_(value) {}

    template <detail::http_temporary_owning_char_string string>
    borrowed_text(string&&) = delete;

    constexpr borrowed_text& operator=(std::string_view value) noexcept {
        value_ = value;
        return *this;
    }

    constexpr borrowed_text& operator=(const char* value) noexcept {
        value_ = detail::http_borrowed_c_string_view(value);
        return *this;
    }

    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text& operator=(
        const std::basic_string<char, traits_type, allocator_type>& value) noexcept {
        value_ = std::string_view(value);
        return *this;
    }

    template <detail::http_temporary_owning_char_string string>
    borrowed_text& operator=(string&&) = delete;

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr const char* data() const noexcept {
        return value_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return value_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return value_.empty();
    }

    friend constexpr bool operator==(borrowed_text left, borrowed_text right) noexcept {
        return left.value_ == right.value_;
    }

    friend constexpr bool operator==(borrowed_text left, std::string_view right) noexcept {
        return left.value_ == right;
    }

    friend constexpr bool operator==(borrowed_text left, const char* right) noexcept {
        return left.value_ == detail::http_borrowed_c_string_view(right);
    }

    template <typename traits_type, typename allocator_type>
    friend constexpr bool operator==(
        borrowed_text left, const std::basic_string<char, traits_type, allocator_type>& right) noexcept {
        return left.value_ == std::string_view(right);
    }

private:
    std::string_view value_;
};

static_assert(sizeof(borrowed_text) == sizeof(std::string_view));

// Zero-copy element span held by an option or message-view struct. Accepts
// borrowed spans and contiguous owning-container lvalues while rejecting
// temporary owning containers (std::vector, std::array) and
// initializer_list values whose backing storage would expire before the struct
// is consumed.
template <typename element_type>
class borrowed_span final {
public:
    constexpr borrowed_span() noexcept = default;

    constexpr borrowed_span(std::span<const element_type> values) noexcept
        : values_(values) {}

    constexpr borrowed_span(std::span<element_type> values) noexcept
        : values_(values) {}

    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) &&
                std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
                std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span(range_type&& values) noexcept
        : values_(std::ranges::data(values), std::ranges::size(values)) {}

    template <detail::http_temporary_owning_range<element_type> range_type>
    borrowed_span(range_type&&) = delete;

    constexpr borrowed_span(std::initializer_list<element_type>) = delete;

    constexpr borrowed_span& operator=(std::span<const element_type> values) noexcept {
        values_ = values;
        return *this;
    }

    constexpr borrowed_span& operator=(std::span<element_type> values) noexcept {
        values_ = values;
        return *this;
    }

    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) &&
                (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) &&
                std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
                std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span& operator=(range_type&& values) noexcept {
        values_ = std::span<const element_type>(std::ranges::data(values), std::ranges::size(values));
        return *this;
    }

    template <detail::http_temporary_owning_range<element_type> range_type>
    borrowed_span& operator=(range_type&&) = delete;

    constexpr borrowed_span& operator=(std::initializer_list<element_type>) = delete;

    [[nodiscard]] constexpr std::span<const element_type> view() const noexcept {
        return values_;
    }

    [[nodiscard]] constexpr operator std::span<const element_type>() const noexcept {
        return values_;
    }

    [[nodiscard]] constexpr const element_type* data() const noexcept {
        return values_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] constexpr auto begin() const noexcept {
        return values_.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept {
        return values_.end();
    }

    [[nodiscard]] constexpr const element_type& front() const noexcept {
        return values_.front();
    }

    [[nodiscard]] constexpr const element_type& back() const noexcept {
        return values_.back();
    }

    [[nodiscard]] constexpr const element_type& operator[](std::size_t index) const noexcept {
        return values_[index];
    }

private:
    std::span<const element_type> values_{};
};

}  // namespace ruvia

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

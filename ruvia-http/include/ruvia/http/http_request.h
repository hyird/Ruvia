#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
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

// A transport-neutral 16-bit port value shared by HTTP authorities and Web
// endpoint configurations. Zero is representable; each consumer owns its
// zero-port policy. Absence remains optional, distinct from an explicit zero.
class port_number final {
public:
    constexpr port_number() noexcept = default;

    // Numeric configuration remains convenient, but the original integer is
    // checked before narrowing. There is no implicit conversion back to an
    // integer: unwrap value() only at a native or formatting boundary.
    template <std::integral integer_type>
    constexpr port_number(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint16_t>(value)
                     : throw std::invalid_argument("port number must be in 0..65535")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint16_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<port_number> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return port_number(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(port_number, port_number) = default;

private:
    struct validated_tag_type {};

    constexpr port_number(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_{0};
};

static_assert(std::is_trivially_copyable_v<port_number>);
static_assert(sizeof(port_number) == sizeof(std::uint16_t));

enum class ip_address_family : std::uint8_t {
    ipv4,
    ipv6,
};

// A numeric IPv4 or IPv6 address held as network-order bytes, independent of
// any socket library. An IPv6 address may carry a numeric zone (RFC 4007 §11)
// for link-local peers; zero means none. An IPv4-mapped IPv6 address
// (RFC 4291 §2.5.5.2) is a distinct IPv6 value here; a consumer that treats it
// as its IPv4 form states so.
class ip_address final {
public:
    // 0.0.0.0
    constexpr ip_address() noexcept = default;

    [[nodiscard]] static constexpr ip_address ipv4(const std::array<std::uint8_t, 4>& bytes) noexcept {
        ip_address result;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            result.bytes_[i] = bytes[i];
        }
        return result;
    }

    [[nodiscard]] static constexpr ip_address ipv6(
        const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept {
        ip_address result;
        result.bytes_ = bytes;
        result.scope_id_ = scope_id;
        result.family_ = ip_address_family::ipv6;
        return result;
    }

    // A complete address literal: dotted-decimal IPv4 without leading zeros,
    // or RFC 4291 §2.2 IPv6 text with an optional numeric "%zone". Brackets,
    // ports, surrounding space and embedded NUL yield std::nullopt.
    [[nodiscard]] static std::optional<ip_address> parse(std::string_view text) noexcept;

    [[nodiscard]] constexpr ip_address_family family() const noexcept {
        return family_;
    }

    // 4 bytes for IPv4, 16 for IPv6, in network byte order.
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const std::uint8_t>(bytes_.data(), family_ == ip_address_family::ipv4 ? 4 : 16);
    }
    std::span<const std::uint8_t> bytes() const&& = delete;

    [[nodiscard]] constexpr std::uint32_t scope_id() const noexcept {
        return scope_id_;
    }
    [[nodiscard]] constexpr bool is_ipv4_mapped() const noexcept {
        return family_ == ip_address_family::ipv6 && scope_id_ == 0 &&
               bytes_[0] == 0 && bytes_[1] == 0 && bytes_[2] == 0 && bytes_[3] == 0 &&
               bytes_[4] == 0 && bytes_[5] == 0 && bytes_[6] == 0 && bytes_[7] == 0 &&
               bytes_[8] == 0 && bytes_[9] == 0 && bytes_[10] == 0xff && bytes_[11] == 0xff;
    }

    [[nodiscard]] constexpr ip_address normalized_ipv4() const noexcept {
        if (is_ipv4_mapped()) {
            return ipv4({bytes_[12], bytes_[13], bytes_[14], bytes_[15]});
        }
        return *this;
    }

    // RFC 5952 text: dotted decimal for IPv4; for IPv6 lowercase hex with the
    // longest zero run compressed, IPv4-mapped as "::ffff:a.b.c.d", and a zone
    // as "%<scope>".
    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const ip_address&, const ip_address&) noexcept = default;
    friend constexpr auto operator<=>(const ip_address&, const ip_address&) noexcept = default;

private:
    ip_address_family family_{ip_address_family::ipv4};
    std::array<std::uint8_t, 16> bytes_{};
    std::uint32_t scope_id_{0};
};

// An address and port of one transport endpoint. Port zero means the port is
// not known (for example a forwarded client address that recorded none).
class ip_endpoint final {
public:
    constexpr ip_endpoint() noexcept = default;
    constexpr ip_endpoint(ip_address address, std::uint16_t port) noexcept
        : address_(address),
          port_(port) {}

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }

    [[nodiscard]] constexpr std::uint16_t port() const noexcept {
        return port_;
    }

    friend constexpr bool operator==(const ip_endpoint&, const ip_endpoint&) noexcept = default;

private:
    ip_address address_{};
    std::uint16_t port_{0};
};

static_assert(std::is_trivially_copyable_v<ip_address>);
static_assert(std::is_trivially_copyable_v<ip_endpoint>);

// An IPv4 or IPv6 CIDR prefix block (or bare address with full-width prefix).
// IPv4 blocks are stored in IPv4-mapped IPv6 form so 10.0.0.0/8 matches both
// 10.1.2.3 and ::ffff:10.1.2.3.
class ip_cidr final {
public:
    constexpr ip_cidr() noexcept = default;

    [[nodiscard]] static std::optional<ip_cidr> parse(std::string_view text) noexcept;

    [[nodiscard]] bool contains(const ip_address& address) const noexcept;
    [[nodiscard]] bool contains(std::string_view address_text) const noexcept;

    friend constexpr bool operator==(const ip_cidr&, const ip_cidr&) noexcept = default;

private:
    std::array<std::uint8_t, 16> network_{};
    std::uint8_t prefix_bits_{128};
};

static_assert(std::is_trivially_copyable_v<ip_cidr>);

// Maximum decimal digits of a std::uint64_t (18446744073709551615).
inline constexpr std::size_t http_max_unsigned_decimal_size = 20;

[[nodiscard]] constexpr std::size_t http_unsigned_decimal_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

// The single integer formatter: writes the decimal form of `value` at the start
// of `output` and returns that prefix. Writers that measured an exact slice
// with http_unsigned_decimal_size() copy the result instead of formatting in
// place, so no pointer past the slice is ever formed.
[[nodiscard]] inline std::string_view http_format_unsigned(
    std::span<char, http_max_unsigned_decimal_size> output, std::uint64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

// Sign plus the 19 digits of the most negative std::int64_t.
inline constexpr std::size_t http_max_signed_decimal_size = 20;

[[nodiscard]] inline std::string_view http_format_signed(
    std::span<char, http_max_signed_decimal_size> output, std::int64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

inline void http_append_unsigned(std::string& output, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    output.append(http_format_unsigned(digits, value));
}

inline void http_append_signed(std::string& output, std::int64_t value) {
    std::array<char, http_max_signed_decimal_size> digits;
    output.append(http_format_signed(digits, value));
}

// Append the shortest decimal form of an arithmetic value; `error_message` is
// thrown as std::logic_error if formatting fails. Integers go through
// http_append_signed() / http_append_unsigned(), which cannot fail.
template <typename number_type>
void http_append_formatted_number(
    std::string& output, number_type value, [[maybe_unused]] const char* error_message) {
    if constexpr (std::is_integral_v<number_type> && std::is_signed_v<number_type>) {
        http_append_signed(output, value);
    } else if constexpr (std::is_integral_v<number_type>) {
        http_append_unsigned(output, value);
    } else {
        std::array<char, 64> buffer;
        const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (ec != std::errc{}) {
            throw std::logic_error(error_message);
        }
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

// Floating-point variant that rejects NaN and infinities with
// std::invalid_argument before touching `output`.
template <typename number_type>
void http_append_formatted_finite_number(std::string& output, number_type value,
    const char* finite_error_message, const char* format_error_message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(finite_error_message);
    }
    http_append_formatted_number(output, value, format_error_message);
}

struct utf8_sequence final {
    // Bytes of the well-formed sequence, or of the maximal subpart (Unicode 3.9,
    // definition D93b) that precedes the first ill-formed byte; always >= 1.
    std::size_t length_;
    bool well_formed_;
};

// Requires i < input.size() and input[i] >= 0x80 when interpreted as unsigned.
// Classifies the UTF-8 sequence beginning at input[i]
// under the Unicode 3.9 Table 3-7 constraints: no overlong forms (C0/C1, E0 80-9F,
// F0 80-8F), no UTF-16 surrogate code points (ED A0-BF), nothing above U+10FFFF
// (F4 90-.., F5+), and no stray or truncated continuation bytes. The single owner
// of UTF-8 well-formedness: WebSocket text and close reasons, SSE fields, JSON
// parsing (rejects), JSON writing (one U+FFFD per maximal subpart), compile-time
// model wire names, JWT names and MariaDB identifier character counts.
[[nodiscard]] constexpr utf8_sequence scan_utf8_sequence(std::string_view input, std::size_t i) noexcept {
    const auto lead = static_cast<unsigned char>(input[i]);
    std::size_t trail = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trail = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trail = 2;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trail = 3;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return {1, false};  // 0x80-0xC1 (bare continuation / overlong lead) or 0xF5-0xFF
    }
    for (std::size_t k = 1; k <= trail; ++k) {
        if (i + k >= input.size()) {
            return {k, false};
        }
        const auto byte = static_cast<unsigned char>(input[i + k]);
        if (byte < low || byte > high) {
            return {k, false};
        }
        low = 0x80;
        high = 0xBF;
    }
    return {trail + 1, true};
}

[[nodiscard]] constexpr bool is_valid_utf8(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size();) {
        if (static_cast<unsigned char>(value[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto sequence = scan_utf8_sequence(value, i);
        if (!sequence.well_formed_) {
            return false;
        }
        i += sequence.length_;
    }
    return true;
}

namespace detail {

[[nodiscard]] inline std::size_t http_forward_encoded_groups(
    const void* output, const void* input, std::size_t groups) noexcept {
    const auto output_address = reinterpret_cast<std::uintptr_t>(output);
    const auto input_address = reinterpret_cast<std::uintptr_t>(input);
    if (output_address >= input_address) {
        return 0;
    }
    const auto distance = input_address - output_address;
    return distance < groups ? static_cast<std::size_t>(distance) : groups;
}

}  // namespace detail

// Decode one hex digit (either case); returns -1 for a non-hex character.
[[nodiscard]] constexpr int http_decode_hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Encode the low nibble of `value` as a hex digit. Two case-specific functions
// keep the case selection at the call site: percent-encoding needs uppercase
// (RFC 3986 section 2.1), token/hash encoders conventionally use lowercase.
[[nodiscard]] constexpr char http_lower_hex_digit(int value) noexcept {
    return "0123456789abcdef"[value & 0x0F];
}

[[nodiscard]] constexpr char http_upper_hex_digit(int value) noexcept {
    return "0123456789ABCDEF"[value & 0x0F];
}

// The single byte-sequence to lowercase hex encoder: writes exactly
// 2 * bytes.size() digits starting at `output` and returns the end. `output`
// may overlap `bytes` anywhere, including in place.
constexpr char* http_encode_lower_hex(char* output, std::span<const unsigned char> bytes) noexcept {
    const auto encode_byte = [output, bytes](std::size_t index) noexcept {
        const auto byte = bytes[index];
        output[2 * index] = http_lower_hex_digit(byte >> 4);
        output[2 * index + 1] = http_lower_hex_digit(byte);
    };
    // Constant evaluation cannot make char output alias unsigned char input.
    auto forward_bytes = bytes.size();
    if (!std::is_constant_evaluated()) {
        forward_bytes = detail::http_forward_encoded_groups(output, bytes.data(), bytes.size());
    }
    for (auto index = bytes.size(); index != forward_bytes;) {
        encode_byte(--index);
    }
    for (std::size_t index = 0; index != forward_bytes; ++index) {
        encode_byte(index);
    }
    return output + 2 * bytes.size();
}

// Text is encoded byte by byte, like its unsigned char representation.
inline char* http_encode_lower_hex(char* output, std::string_view bytes) noexcept {
    return http_encode_lower_hex(
        output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// `bytes` may alias `output`, up to and including its terminating NUL at
// output.data()[output.size()]; an aliased input is rebased by offset after
// the resize, which may reallocate. Throws std::length_error when the result
// cannot fit the string.
inline void http_append_lower_hex(std::string& output, std::span<const unsigned char> bytes) {
    const auto start = output.size();
    if (bytes.size() > (output.max_size() - start) / 2) {
        throw std::length_error("hex output length is not representable");
    }
    const auto first = reinterpret_cast<std::uintptr_t>(output.data());
    const auto bytes_address = reinterpret_cast<std::uintptr_t>(bytes.data());
    const bool aliased = !bytes.empty() && bytes_address >= first && bytes_address - first <= start;
    const auto offset = aliased ? static_cast<std::size_t>(bytes_address - first) : 0;
    output.resize(start + bytes.size() * 2);
    // An aliased source ends at the latest at the old terminating NUL, index
    // start, which resize rewrote with the same NUL.
    const auto source =
        aliased ? std::span(reinterpret_cast<const unsigned char*>(output.data()) + offset, bytes.size()) : bytes;
    http_encode_lower_hex(output.data() + start, source);
}

inline void http_append_lower_hex(std::string& output, std::string_view bytes) {
    http_append_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// RFC 4648 section 4 (standard) and section 5 (URL-safe) alphabets.
inline constexpr std::string_view http_base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view http_base64_url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum class http_base64_charset : std::uint8_t {
    standard,  // RFC 4648 section 4: '+' and '/'
    url_safe,  // RFC 4648 section 5: '-' and '_'
};

enum class http_base64_padding : std::uint8_t {
    required,   // '=' completes the final quantum; length is a multiple of 4
    forbidden,  // unpadded (RFC 4648 section 3.2), e.g. JWS base64url
};

// Throws std::length_error when the output length cannot fit std::size_t.
[[nodiscard]] constexpr std::size_t http_base64_encoded_size(
    std::size_t input_size, http_base64_padding padding = http_base64_padding::required) {
    const auto groups = input_size / 3;
    const auto remainder = input_size % 3;
    const std::size_t tail =
        remainder == 0 ? 0 : (padding == http_base64_padding::required ? 4 : remainder + 1);
    if (groups > ((std::numeric_limits<std::size_t>::max)() - tail) / 4) {
        throw std::length_error("base64 output length is not representable");
    }
    return groups * 4 + tail;
}

// The single base64 encoder. `output` must have room for
// http_base64_encoded_size(input.size(), padding) characters and may overlap
// `input` anywhere, including in place.
inline void http_base64_encode(char* output, std::span<const std::uint8_t> input,
    http_base64_charset charset = http_base64_charset::standard,
    http_base64_padding padding = http_base64_padding::required) noexcept {
    const auto alphabet =
        charset == http_base64_charset::url_safe ? http_base64_url_alphabet : http_base64_alphabet;
    const bool padded = padding == http_base64_padding::required;
    // Group k reads input[3k, 3k + 3) before writing output[4k, 4k + 4).
    const auto encode_group = [output, input, alphabet, padded](std::size_t group) noexcept {
        const auto bytes = input.subspan(group * 3, (std::min)(input.size() - group * 3, std::size_t{3}));
        auto value = static_cast<std::uint32_t>(bytes[0]) << 16;
        if (bytes.size() > 1) {
            value |= static_cast<std::uint32_t>(bytes[1]) << 8;
        }
        if (bytes.size() > 2) {
            value |= static_cast<std::uint32_t>(bytes[2]);
        }
        auto* const digits = output + group * 4;
        digits[0] = alphabet[(value >> 18) & 0x3F];
        digits[1] = alphabet[(value >> 12) & 0x3F];
        if (bytes.size() > 1) {
            digits[2] = alphabet[(value >> 6) & 0x3F];
        } else if (padded) {
            digits[2] = '=';
        }
        if (bytes.size() > 2) {
            digits[3] = alphabet[value & 0x3F];
        } else if (padded) {
            digits[3] = '=';
        }
    };
    const auto groups = input.size() / 3 + (input.size() % 3 == 0 ? 0 : 1);
    const auto forward_groups = detail::http_forward_encoded_groups(output, input.data(), groups);
    for (auto group = groups; group != forward_groups;) {
        encode_group(--group);
    }
    for (std::size_t group = 0; group != forward_groups; ++group) {
        encode_group(group);
    }
}

// Decode one standard-alphabet character to its 6-bit value; returns -1 for
// any other character, including the '=' pad.
[[nodiscard]] constexpr int http_base64_decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// URL-safe counterpart of http_base64_decode_char ('-' and '_' replace '+'
// and '/').
[[nodiscard]] constexpr int http_base64_url_decode_char(char c) noexcept {
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return c == '+' || c == '/' ? -1 : http_base64_decode_char(c);
}

// Upper bound of the decoded size of `input_size` characters; exact for valid
// unpadded input.
[[nodiscard]] constexpr std::size_t http_base64_decoded_size_bound(std::size_t input_size) noexcept {
    const auto remainder = input_size % 4;
    return input_size / 4 * 3 + (remainder == 0 ? 0 : remainder - 1);
}

// The single base64 decoder; allocation-free. Accepts only canonical input:
// characters of `charset`, '=' exactly as `padding` requires, no final
// quantum of one character, and zero unused bits in the final quantum
// (RFC 4648 section 3.5). Returns the decoded byte count, or nullopt for
// invalid input or an `output` too small for the decoded bytes. `output` may
// start at or before `input` (in-place decoding) or past its encoded
// characters; one starting inside them, after the first, would overwrite
// unread input and is rejected with nullopt before any write. Length and
// ownership policy belongs to the caller.
[[nodiscard]] constexpr std::optional<std::size_t> http_base64_decode(std::string_view input,
    std::span<std::uint8_t> output, http_base64_charset charset, http_base64_padding padding) noexcept {
    auto data = input;
    if (padding == http_base64_padding::required) {
        if (input.size() % 4 != 0) {
            return std::nullopt;
        }
        if (data.ends_with("==")) {
            data.remove_suffix(2);
        } else if (data.ends_with('=')) {
            data.remove_suffix(1);
        }
        // Padding only completes a partial final quantum.
        if (data.size() != input.size() && data.size() % 4 + (input.size() - data.size()) != 4) {
            return std::nullopt;
        }
    }
    const auto remainder = data.size() % 4;
    if (remainder == 1) {
        return std::nullopt;
    }
    const auto decoded_size = http_base64_decoded_size_bound(data.size());
    if (decoded_size > output.size()) {
        return std::nullopt;
    }
    // Overlap needs reinterpret_cast, which constant evaluation excludes.
    if (!std::is_constant_evaluated() && !data.empty()) {
        const auto encoded = reinterpret_cast<std::uintptr_t>(data.data());
        const auto output_address = reinterpret_cast<std::uintptr_t>(output.data());
        if (output_address > encoded && output_address - encoded < data.size()) {
            return std::nullopt;
        }
    }
    const auto value_of = [charset](char c) noexcept {
        return charset == http_base64_charset::url_safe ? http_base64_url_decode_char(c)
                                                        : http_base64_decode_char(c);
    };
    std::size_t out = 0;
    std::size_t in = 0;
    for (; data.size() - in >= 4; in += 4) {
        const auto first = value_of(data[in]);
        const auto second = value_of(data[in + 1]);
        const auto third = value_of(data[in + 2]);
        const auto fourth = value_of(data[in + 3]);
        if ((first | second | third | fourth) < 0) {
            return std::nullopt;
        }
        output[out++] = static_cast<std::uint8_t>((first << 2) | (second >> 4));
        output[out++] = static_cast<std::uint8_t>((second << 4) | (third >> 2));
        output[out++] = static_cast<std::uint8_t>((third << 6) | fourth);
    }
    if (remainder == 0) {
        return out;
    }
    const auto first = value_of(data[in]);
    const auto second = value_of(data[in + 1]);
    const auto third = remainder == 3 ? value_of(data[in + 2]) : 0;
    if ((first | second | third) < 0 || (remainder == 2 && (second & 0x0F) != 0) ||
        (remainder == 3 && (third & 0x03) != 0)) {
        return std::nullopt;
    }
    output[out++] = static_cast<std::uint8_t>((first << 2) | (second >> 4));
    if (remainder == 3) {
        output[out++] = static_cast<std::uint8_t>((second << 4) | (third >> 2));
    }
    return out;
}

enum class url_decode_mode : std::uint8_t { percent,
    form };

// Percent mode treats '+' literally; form mode decodes '+' as a space.
[[nodiscard]] inline bool has_url_encoding(std::string_view value, url_decode_mode mode) noexcept {
    // Keep short components and early escapes on the scalar path; library searches
    // can scan the remaining literal text in larger chunks.
    constexpr std::size_t scalar_scan_limit = 64;
    constexpr std::size_t scalar_prefix_size = 32;
    const auto prefix = value.substr(0, value.size() <= scalar_scan_limit ? value.size() : scalar_prefix_size);
    const bool encoded = mode == url_decode_mode::form
                             ? std::ranges::any_of(prefix, [](char c) noexcept { return c == '%' || c == '+'; })
                             : std::ranges::any_of(prefix, [](char c) noexcept { return c == '%'; });
    if (encoded) {
        return true;
    }
    if (prefix.size() == value.size()) {
        return false;
    }
    value.remove_prefix(prefix.size());
    return (mode == url_decode_mode::form ? value.find_first_of("%+") : value.find('%')) !=
           std::string_view::npos;
}

// Decodes the pct-encoded triplet starting at input[index] (RFC 3986 2.1):
// '%' followed by two hex digits. Returns no value when the triplet is
// truncated, does not start with '%', or contains a non-hex digit.
[[nodiscard]] constexpr std::optional<unsigned char> http_try_decode_percent_byte(
    std::string_view input, std::size_t index) noexcept {
    if (index >= input.size() || input.size() - index < 3 || input[index] != '%') {
        return std::nullopt;
    }
    const auto high = http_decode_hex_nibble(input[index + 1]);
    const auto low = http_decode_hex_nibble(input[index + 2]);
    if (high < 0 || low < 0) {
        return std::nullopt;
    }
    return static_cast<unsigned char>((high << 4) | low);
}

// Appends the canonical uppercase pct-encoded triplet for byte. Callers own the
// policy deciding which bytes need escaping. Failure leaves output unchanged.
inline void http_append_percent_byte(std::string& output, unsigned char byte) {
    const char triplet[]{'%', http_upper_hex_digit(byte >> 4), http_upper_hex_digit(byte)};
    output.append(triplet, sizeof(triplet));
}

// Repairs an application-supplied RFC 3986 URI-reference (for example a
// Location field value) by percent-encoding every byte that cannot appear
// literally at its position. Well-formed pct-encoded triplets and every legal
// byte are kept byte for byte, so a correctly encoded reference is never
// encoded twice (RFC 3986 section 2.4). '[' and ']' stay literal only as the
// authority's IP-literal delimiters, ':' is encoded in the first segment of a
// relative-path reference (section 4.2), '@' inside userinfo is encoded so only
// the delimiting '@' remains (section 3.2.1), and only the first '#' starts the
// fragment. Returns `reference` itself, without allocating, when it needs no
// encoding; otherwise the encoded reference replaces `storage`'s contents and
// the result views `storage`. Failure preserves storage's original contents.
// `reference` may alias `storage`. Keep the input owner alive until the result
// is consumed; owning-string temporaries are rejected even when their particular
// contents would require transformation.
[[nodiscard]] std::string_view percent_encode_uri_reference(borrowed_text reference, std::string& storage);

namespace detail {
template <typename visitor_type>
[[nodiscard]] bool dispatch_url_encoded_pair_visitor(
    visitor_type& visitor, std::string_view name, std::string_view value) {
    if constexpr (requires {
                      { visitor(name, value) } -> std::convertible_to<bool>;
                  }) {
        return static_cast<bool>(visitor(name, value));
    } else {
        visitor(name, value);
        return true;
    }
}

}  // namespace detail

[[nodiscard]] inline bool validate_url_encoding(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            continue;
        }
        if (!http_try_decode_percent_byte(value, i)) {
            return false;
        }
        i += 2;
    }
    return true;
}

// Decodes one URL component. Returns `input` itself, without allocating, when
// it holds nothing to decode; otherwise the decoded bytes replace `storage`'s
// contents and the result views `storage`. Percent mode treats '+' literally
// and returns no value for malformed percent encoding (RFC 3986 pct-encoded),
// leaving `storage` unchanged. Form mode converts '+' to a space and, like the
// WHATWG percent-decode algorithm used by application/x-www-form-urlencoded,
// keeps a '%' that does not start a valid escape as a literal byte. `input`
// may alias `storage`. Keep its owner alive until the result is consumed;
// owning-string temporaries are rejected regardless of their contents.
[[nodiscard]] std::optional<std::string_view> decode_url_component(
    borrowed_text text, std::string& storage, url_decode_mode mode = url_decode_mode::percent);

// Origin-form path identity, the one canonical form shared by route matching,
// path-scoped middleware and fallbacks, and decoded route parameters. A path
// splits into segments on literal '/' only, and each segment is its
// percent-decoded octets. Hex case and encoded unreserved characters therefore
// never make two spellings differ (RFC 3986 6.2.2.1, 6.2.2.2), and an encoded
// "%2F" stays inside its segment instead of becoming a boundary. The form is
// deliberately coarser than RFC 3986 equivalence: an encoded reserved
// character equals its literal ("%3A" and ":"), because a parameter decoded
// from either spelling reads the same value. RFC 9110 4.2.3 only forbids
// splitting equivalent URIs across resources, not merging non-equivalent ones.
//
// The canonical unit at path[index] (index < path.size()) is its literal or
// decoded octet (0-255), uri_path_separator_unit for a literal '/', or -1 for
// a malformed escape; next_uri_path_unit returns it and advances index past it.
inline constexpr int uri_path_separator_unit = 0x100;

[[nodiscard]] constexpr int next_uri_path_unit(std::string_view path, std::size_t& index) noexcept {
    const char c = path[index];
    if (c == '/') {
        ++index;
        return uri_path_separator_unit;
    }
    if (c == '%') {
        const auto byte = http_try_decode_percent_byte(path, index);
        if (!byte) {
            ++index;
            return -1;
        }
        index += 3;
        return *byte;
    }
    ++index;
    return static_cast<unsigned char>(c);
}

// Three-way order of two paths by their canonical units: negative, zero or
// positive like std::string_view::compare. A total order over well-formed
// paths, consistent with uri_paths_equivalent, for sorted lookups.
[[nodiscard]] constexpr int compare_uri_paths(std::string_view left, std::string_view right) noexcept {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const auto left_unit = next_uri_path_unit(left, left_index);
        const auto right_unit = next_uri_path_unit(right, right_index);
        if (left_unit != right_unit) {
            return left_unit < right_unit ? -1 : 1;
        }
    }
    if (left_index < left.size()) {
        return 1;
    }
    return right_index < right.size() ? -1 : 0;
}

// Whether two paths (or two segments) have the same canonical form. Malformed
// escapes never compare equal. Spellings without '%' compare byte for byte.
[[nodiscard]] constexpr bool uri_paths_equivalent(std::string_view left, std::string_view right) noexcept {
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const auto left_unit = next_uri_path_unit(left, left_index);
        if (left_unit < 0 || left_unit != next_uri_path_unit(right, right_index)) {
            return false;
        }
    }
    return left_index == left.size() && right_index == right.size();
}

// Visit raw, borrowed name/value views for each non-empty pair, preserving repeated
// names in input order. A bool-returning visitor returning false stops iteration
// and makes this return false; a void visitor always traverses all pairs. Pair
// traversal does not validate percent escapes. Input storage must outlive each view.
template <typename visitor_type>
[[nodiscard]] bool visit_url_encoded_pairs(std::string_view input, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    while (!input.empty()) {
        const auto pair_end = input.find('&');
        const auto pair = pair_end == std::string_view::npos ? input : input.substr(0, pair_end);

        // Skip empty segments ("&&", leading/trailing "&") rather than emitting a phantom
        // ("", "") pair : a segment with no bytes carries no field.
        if (!pair.empty()) {
            const auto equals = pair.find('=');
            const auto name = equals == std::string_view::npos ? pair : pair.substr(0, equals);
            const auto value =
                equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            if (!detail::dispatch_url_encoded_pair_visitor(visitor_ref, name, value)) {
                return false;
            }
        }

        if (pair_end == std::string_view::npos) {
            return true;
        }
        input.remove_prefix(pair_end + 1);
    }
    return true;
}

// A validated HTTP status-code value. The complete 100..599 extension space is
// representable, while arbitrary integers cannot leak into protocol APIs.
class http_status_code final {
public:
    // Validate the original integer before narrowing to the stored wire value.
    template <std::integral integer_type>
    [[nodiscard]] static constexpr http_status_code from_value(integer_type value) {
        if (!is_valid_value(value)) {
            throw std::invalid_argument("HTTP status code must be in 100..599");
        }
        return http_status_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_status_code> try_from_value(
        integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_status_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        const auto integer_value = +value;
        return integer_value >= 100 && integer_value <= 599;
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool is_informational() const noexcept {
        return value_ < 200;
    }

    [[nodiscard]] constexpr bool is_successful() const noexcept {
        return value_ >= 200 && value_ < 300;
    }

    [[nodiscard]] constexpr bool is_redirection() const noexcept {
        return value_ >= 300 && value_ < 400;
    }

    [[nodiscard]] constexpr bool is_client_error() const noexcept {
        return value_ >= 400 && value_ < 500;
    }

    [[nodiscard]] constexpr bool is_server_error() const noexcept {
        return value_ >= 500;
    }

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return value_ >= 400;
    }

    [[nodiscard]] constexpr bool is_final() const noexcept {
        return value_ >= 200;
    }

    friend constexpr auto operator<=>(http_status_code, http_status_code) = default;

private:
    struct validated_tag_type {};

    constexpr http_status_code(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<http_status_code>);
static_assert(sizeof(http_status_code) == sizeof(std::uint16_t));

namespace detail {

inline constexpr std::size_t http_status_code_token_size = 3;
using http_status_code_token_type = std::array<char, http_status_code_token_size>;

[[nodiscard]] inline constexpr http_status_code_token_type http_status_code_token(
    http_status_code status) noexcept {
    const auto value = status.value();
    return {static_cast<char>('0' + value / 100), static_cast<char>('0' + (value / 10) % 10),
        static_cast<char>('0' + value % 10)};
}

[[nodiscard]] inline constexpr std::string_view http_status_code_token_view(
    const http_status_code_token_type& token) noexcept {
    return std::string_view(token.data(), token.size());
}

[[nodiscard]] std::string_view http_status_code_token_view(http_status_code_token_type&&) = delete;

}  // namespace detail

// Stable RFC-assigned names are defined once here. Unknown extension codes and
// temporary draft registrations remain available through from_value() and
// try_from_value() without turning an unstable name into framework API.
namespace http_status {

inline constexpr auto continue_value = http_status_code::from_value(100);
inline constexpr auto switching_protocols = http_status_code::from_value(101);
inline constexpr auto processing = http_status_code::from_value(102);
inline constexpr auto early_hints = http_status_code::from_value(103);

inline constexpr auto ok = http_status_code::from_value(200);
inline constexpr auto created = http_status_code::from_value(201);
inline constexpr auto accepted = http_status_code::from_value(202);
inline constexpr auto non_authoritative_information = http_status_code::from_value(203);
inline constexpr auto no_content = http_status_code::from_value(204);
inline constexpr auto reset_content = http_status_code::from_value(205);
inline constexpr auto partial_content = http_status_code::from_value(206);
inline constexpr auto multi_status = http_status_code::from_value(207);
inline constexpr auto already_reported = http_status_code::from_value(208);
inline constexpr auto im_used = http_status_code::from_value(226);

inline constexpr auto multiple_choices = http_status_code::from_value(300);
inline constexpr auto moved_permanently = http_status_code::from_value(301);
inline constexpr auto found = http_status_code::from_value(302);
inline constexpr auto see_other = http_status_code::from_value(303);
inline constexpr auto not_modified = http_status_code::from_value(304);
inline constexpr auto use_proxy = http_status_code::from_value(305);
inline constexpr auto temporary_redirect = http_status_code::from_value(307);
inline constexpr auto permanent_redirect = http_status_code::from_value(308);

inline constexpr auto bad_request = http_status_code::from_value(400);
inline constexpr auto unauthorized = http_status_code::from_value(401);
inline constexpr auto payment_required = http_status_code::from_value(402);
inline constexpr auto forbidden = http_status_code::from_value(403);
inline constexpr auto not_found = http_status_code::from_value(404);
inline constexpr auto method_not_allowed = http_status_code::from_value(405);
inline constexpr auto not_acceptable = http_status_code::from_value(406);
inline constexpr auto proxy_authentication_required = http_status_code::from_value(407);
inline constexpr auto request_timeout = http_status_code::from_value(408);
inline constexpr auto conflict = http_status_code::from_value(409);
inline constexpr auto gone = http_status_code::from_value(410);
inline constexpr auto length_required = http_status_code::from_value(411);
inline constexpr auto precondition_failed = http_status_code::from_value(412);
inline constexpr auto content_too_large = http_status_code::from_value(413);
inline constexpr auto uri_too_long = http_status_code::from_value(414);
inline constexpr auto unsupported_media_type = http_status_code::from_value(415);
inline constexpr auto range_not_satisfiable = http_status_code::from_value(416);
inline constexpr auto expectation_failed = http_status_code::from_value(417);
inline constexpr auto misdirected_request = http_status_code::from_value(421);
inline constexpr auto unprocessable_content = http_status_code::from_value(422);
inline constexpr auto locked = http_status_code::from_value(423);
inline constexpr auto failed_dependency = http_status_code::from_value(424);
inline constexpr auto too_early = http_status_code::from_value(425);
inline constexpr auto upgrade_required = http_status_code::from_value(426);
inline constexpr auto precondition_required = http_status_code::from_value(428);
inline constexpr auto too_many_requests = http_status_code::from_value(429);
inline constexpr auto request_header_fields_too_large = http_status_code::from_value(431);
inline constexpr auto unavailable_for_legal_reasons = http_status_code::from_value(451);

inline constexpr auto internal_server_error = http_status_code::from_value(500);
inline constexpr auto not_implemented = http_status_code::from_value(501);
inline constexpr auto bad_gateway = http_status_code::from_value(502);
inline constexpr auto service_unavailable = http_status_code::from_value(503);
inline constexpr auto gateway_timeout = http_status_code::from_value(504);
inline constexpr auto http_version_not_supported = http_status_code::from_value(505);
inline constexpr auto variant_also_negotiates = http_status_code::from_value(506);
inline constexpr auto insufficient_storage = http_status_code::from_value(507);
inline constexpr auto loop_detected = http_status_code::from_value(508);
inline constexpr auto not_extended = http_status_code::from_value(510);
inline constexpr auto network_authentication_required = http_status_code::from_value(511);

}  // namespace http_status

// A status code received by a client. RFC 9110 section 15 makes a three-digit
// code outside 100..599 invalid for senders, but a client that receives one
// SHOULD process the response as if it had a 5xx status, so status() is 500
// for a received 000..099 or 600..999 while value() keeps the code as
// received; for 100..599 both agree.
class http_received_status final {
public:
    // Validate the original integer before narrowing the three-digit code.
    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_received_status> try_from_value(
        integer_type value) noexcept {
        if (std::cmp_less(+value, 0) || std::cmp_greater(+value, 999)) {
            return std::nullopt;
        }
        const auto wire_value = static_cast<std::uint16_t>(value);
        if (const auto status = http_status_code::try_from_value(wire_value)) {
            return http_received_status(*status, wire_value);
        }
        return http_received_status(http_status::internal_server_error, wire_value);
    }

    // Parses the received status-code = 3DIGIT of an HTTP/1 status line or an
    // HTTP/2 :status value: exactly three ASCII digits, no sign or spaces.
    [[nodiscard]] static constexpr std::optional<http_received_status> try_parse(
        std::string_view digits) noexcept {
        if (digits.size() != 3) {
            return std::nullopt;
        }
        std::uint16_t value = 0;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            value = static_cast<std::uint16_t>(value * 10 + (digit - '0'));
        }
        return try_from_value(value);
    }

    constexpr http_received_status(http_status_code status) noexcept
        : status_(status),
          value_(status.value()) {}

    // The status the response is processed as.
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // The three-digit code as received.
    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr bool operator==(http_received_status, http_received_status) = default;

private:
    constexpr http_received_status(http_status_code status, std::uint16_t value) noexcept
        : status_(status),
          value_(value) {}

    http_status_code status_;
    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<http_received_status>);

// RFC 9112 reason-phrase is optional HTTP/1 presentation text, not response
// semantics. Stable RFC-assigned codes get a conventional phrase; temporary,
// extension, and unassigned codes deliberately get an empty phrase.
[[nodiscard]] inline constexpr std::string_view http_reason_phrase(http_status_code status) noexcept {
    switch (status.value()) {
        case http_status::continue_value.value():
            return "Continue";
        case http_status::switching_protocols.value():
            return "Switching Protocols";
        case http_status::processing.value():
            return "Processing";
        case http_status::early_hints.value():
            return "Early Hints";
        case http_status::ok.value():
            return "OK";
        case http_status::created.value():
            return "Created";
        case http_status::accepted.value():
            return "Accepted";
        case http_status::non_authoritative_information.value():
            return "Non-Authoritative Information";
        case http_status::no_content.value():
            return "No Content";
        case http_status::reset_content.value():
            return "Reset Content";
        case http_status::partial_content.value():
            return "Partial Content";
        case http_status::multi_status.value():
            return "Multi-Status";
        case http_status::already_reported.value():
            return "Already Reported";
        case http_status::im_used.value():
            return "IM Used";
        case http_status::multiple_choices.value():
            return "Multiple Choices";
        case http_status::moved_permanently.value():
            return "Moved Permanently";
        case http_status::found.value():
            return "Found";
        case http_status::see_other.value():
            return "See Other";
        case http_status::not_modified.value():
            return "Not Modified";
        case http_status::use_proxy.value():
            return "Use Proxy";
        case http_status::temporary_redirect.value():
            return "Temporary Redirect";
        case http_status::permanent_redirect.value():
            return "Permanent Redirect";
        case http_status::bad_request.value():
            return "Bad Request";
        case http_status::unauthorized.value():
            return "Unauthorized";
        case http_status::payment_required.value():
            return "Payment Required";
        case http_status::forbidden.value():
            return "Forbidden";
        case http_status::not_found.value():
            return "Not Found";
        case http_status::method_not_allowed.value():
            return "Method Not Allowed";
        case http_status::not_acceptable.value():
            return "Not Acceptable";
        case http_status::proxy_authentication_required.value():
            return "Proxy Authentication Required";
        case http_status::request_timeout.value():
            return "Request Timeout";
        case http_status::conflict.value():
            return "Conflict";
        case http_status::gone.value():
            return "Gone";
        case http_status::length_required.value():
            return "Length Required";
        case http_status::precondition_failed.value():
            return "Precondition Failed";
        case http_status::content_too_large.value():
            return "Content Too Large";
        case http_status::uri_too_long.value():
            return "URI Too Long";
        case http_status::unsupported_media_type.value():
            return "Unsupported Media Type";
        case http_status::range_not_satisfiable.value():
            return "Range Not Satisfiable";
        case http_status::expectation_failed.value():
            return "Expectation Failed";
        case http_status::misdirected_request.value():
            return "Misdirected Request";
        case http_status::unprocessable_content.value():
            return "Unprocessable Content";
        case http_status::locked.value():
            return "Locked";
        case http_status::failed_dependency.value():
            return "Failed Dependency";
        case http_status::too_early.value():
            return "Too Early";
        case http_status::upgrade_required.value():
            return "Upgrade Required";
        case http_status::precondition_required.value():
            return "Precondition Required";
        case http_status::too_many_requests.value():
            return "Too Many Requests";
        case http_status::request_header_fields_too_large.value():
            return "Request Header Fields Too Large";
        case http_status::unavailable_for_legal_reasons.value():
            return "Unavailable For Legal Reasons";
        case http_status::internal_server_error.value():
            return "Internal Server Error";
        case http_status::not_implemented.value():
            return "Not Implemented";
        case http_status::bad_gateway.value():
            return "Bad Gateway";
        case http_status::service_unavailable.value():
            return "Service Unavailable";
        case http_status::gateway_timeout.value():
            return "Gateway Timeout";
        case http_status::http_version_not_supported.value():
            return "HTTP Version Not Supported";
        case http_status::variant_also_negotiates.value():
            return "Variant Also Negotiates";
        case http_status::insufficient_storage.value():
            return "Insufficient Storage";
        case http_status::loop_detected.value():
            return "Loop Detected";
        case http_status::not_extended.value():
            return "Not Extended";
        case http_status::network_authentication_required.value():
            return "Network Authentication Required";
        default:
            return {};
    }
}

// The redirect status codes whose Location a client may follow
// (RFC 9110 sections 15.4.2-15.4.4, 15.4.8, 15.4.9).
[[nodiscard]] inline constexpr bool is_http_redirect_status(http_status_code status) noexcept {
    return status == http_status::moved_permanently || status == http_status::found ||
           status == http_status::see_other || status == http_status::temporary_redirect ||
           status == http_status::permanent_redirect;
}

namespace detail {

// 101 is a protocol transition rather than an interim progress head. It is
// intentionally owned by a dedicated Upgrade driver instead of either generic
// response-head type.
[[nodiscard]] inline constexpr bool http_interim_status_code_valid(http_status_code status) noexcept {
    return status.is_informational() && status != http_status::switching_protocols;
}

}  // namespace detail

inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
// Field lines per HTTP/1 head or trailer section (the http_field_section_budget
// rule), received and generated, beside the max_http_header_bytes byte cap. The
// smallest field line costs 4 bytes, so the byte cap alone admits ~16 K fields
// whose descriptors outweigh the head; 256 matches Pingora's parser limit.
inline constexpr std::size_t max_http_header_fields = 256;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;
// Bytes one SSE event may buffer while sse_parser assembles it.
inline constexpr std::size_t default_max_sse_event_bytes = std::size_t{16} * 1024 * 1024;

// A protocol byte ceiling with no numeric sentinel: the default is unlimited,
// while a configured limit is always strictly positive.
class protocol_byte_limit final {
public:
    protocol_byte_limit() noexcept = default;

    [[nodiscard]] static protocol_byte_limit unlimited() noexcept {
        return protocol_byte_limit();
    }

    // Validate the original integer before converting to the stored size.
    // Negative inputs must not become effectively unbounded positive ceilings.
    template <std::integral integer_type>
    [[nodiscard]] static protocol_byte_limit limited(integer_type bytes_value) {
        if (!std::in_range<std::size_t>(+bytes_value) || +bytes_value == 0) {
            throw std::invalid_argument("protocol byte limit must be positive and representable");
        }
        return protocol_byte_limit(static_cast<std::size_t>(bytes_value));
    }

    [[nodiscard]] bool is_limited() const noexcept {
        return maximum_.has_value();
    }

    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept {
        return maximum_;
    }

    [[nodiscard]] bool exceeds(std::size_t bytes_value) const noexcept {
        return maximum_.has_value() && bytes_value > *maximum_;
    }

    [[nodiscard]] bool addition_exceeds(std::size_t current, std::size_t added) const noexcept {
        if (added > (std::numeric_limits<std::size_t>::max)() - current) {
            return true;
        }
        return maximum_.has_value() && (current > *maximum_ || added > *maximum_ - current);
    }

    [[nodiscard]] std::size_t read_ceiling() const noexcept {
        return maximum_.value_or((std::numeric_limits<std::size_t>::max)());
    }

private:
    explicit protocol_byte_limit(std::size_t bytes_value) noexcept
        : maximum_(bytes_value) {}

    std::optional<std::size_t> maximum_;
};

// An HTTP protocol primitive rejected input with a response status that a
// server driver may map onto its own application error model. `code` is a
// stable snake_case machine token naming the specific failure (for example
// "missing_host") and must have static storage duration: it is kept as a view.
// The human-readable message is copied into fixed exception storage, so
// reporting never allocates or borrows.
class http_protocol_error final : public std::exception {
public:
    http_protocol_error(
        http_status_code status, std::string_view code, std::string_view message) noexcept
        : status_(status),
          code_(code) {
        const auto size = (std::min)(message.size(), message_.size() - 1);
        if (size != 0) {
            std::memcpy(message_.data(), message.data(), size);
        }
        message_[size] = '\0';
    }

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.data();
    }

private:
    http_status_code status_;
    std::string_view code_;
    std::array<char, 128> message_{};
};

enum class http_parse_error : std::uint8_t {
    header_too_large,
    uri_too_long,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding
};

[[nodiscard]] http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept;

[[nodiscard]] constexpr unsigned char http_ascii_to_lower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool http_ascii_equals_ignore_case(
    std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (http_ascii_to_lower(static_cast<unsigned char>(left[i])) !=
            http_ascii_to_lower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

// The single owner of HTTP field-line syntax (RFC 9110 section 5): token and
// field-content byte classes and the normalized field name/value predicates.
// Field-specific grammars build on these; field-line OWS belongs to wire
// parsers, so normalized values never start or end with SP or HTAB.

// OWS / BWS byte: SP or HTAB (RFC 9110 section 5.6.3).
[[nodiscard]] constexpr bool is_http_ows_char(char c) noexcept {
    return c == ' ' || c == '\t';
}

// Removes only SP and HTAB from the ends of an HTTP field value (OWS,
// RFC 9110 section 5.6.3). The result borrows the caller's input.
[[nodiscard]] constexpr std::string_view http_trim_ows(borrowed_text text) noexcept {
    auto value = text.view();
    while (!value.empty() && is_http_ows_char(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_http_ows_char(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces multi-comparison chains and lets scan loops validate as they move.
inline constexpr std::array<bool, 256> http_token_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table_value[c] = true;
    }
    for (const unsigned char c :
        {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table_value[c] = true;
    }
    return table_value;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> http_field_value_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = 0; c < 256; ++c) {
        table_value[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table_value;
}();

[[nodiscard]] constexpr bool is_http_token_char(unsigned char c) noexcept {
    return http_token_char_table[c];
}

[[nodiscard]] constexpr bool is_http_field_value_char(unsigned char c) noexcept {
    return http_field_value_char_table[c];
}

// Find the first non-token byte. Independent table loads let valid runs avoid
// one loop branch per byte; the final scan preserves the exact stop position.
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= 4) {
        const auto* bytes_value = value.data() + index;
        // Plain bool operands keep the bitwise '&' branchless without the
        // short-circuit warning on call operands.
        const bool first = is_http_token_char(static_cast<unsigned char>(bytes_value[0]));
        const bool second = is_http_token_char(static_cast<unsigned char>(bytes_value[1]));
        const bool third = is_http_token_char(static_cast<unsigned char>(bytes_value[2]));
        const bool fourth = is_http_token_char(static_cast<unsigned char>(bytes_value[3]));
        if (!(first & second & third & fourth)) {
            break;
        }
        index += 4;
    }
    while (index < value.size() && is_http_token_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

namespace detail {
// Keep bulk-scan register use outside inlined short-value callers.
[[nodiscard]] bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept;
}  // namespace detail

// Byte repertoire only. Field parsing separately handles leading/trailing OWS.
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    if (value.size() > 32) {
        return detail::is_valid_long_http_field_value_bytes(value.data(), value.size());
    }
    // Preserve a compile-time loop bound for short-value unrolling.
    const auto size = std::min<std::size_t>(32, value.size());
    for (std::size_t index = 0; index < size; ++index) {
        if (!is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// field-name = token.
[[nodiscard]] inline bool is_valid_http_header_name(std::string_view name) noexcept {
    return !name.empty() && http_token_prefix_size(name) == name.size();
}

// A normalized field value: field-content bytes without leading or trailing
// OWS. Wire parsers that accept field-line OWS trim it before this check.
[[nodiscard]] inline bool is_valid_http_header_value(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (is_http_ows_char(value.front()) || is_http_ows_char(value.back())) {
        return false;
    }
    return is_valid_http_field_value_bytes(value);
}

// 校验单个出站响应字段：上述名称/规范化值语法及响应发送方的已知字段
// 语义（Content-Type、Content-Encoding、Trailer），不分配内存。
// 不应用请求专属字段语义或请求托管字段黑名单，不用于接收方校验。
// 调用方仍负责重复字段、版本相关连接/分帧规则、应用禁改字段政策和
// 完整响应约束；返回 true 不代表该字段在任意响应上下文中都可发送。
[[nodiscard]] bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept;

}  // namespace ruvia

#pragma once

namespace ruvia {

// Index of the next `delimiter` at or after `start` that is outside an RFC 9110
// section 5.6.4 quoted-string, or value.size() when there is none. A quoted-pair
// (`\"`) does not end the string. This is the only quote-aware delimiter scan;
// every list and parameter visitor below is built on it.
[[nodiscard]] constexpr std::size_t http_find_unquoted_delimiter(
    std::string_view value, std::size_t start, char delimiter) noexcept {
    if (start >= value.size()) {
        return value.size();
    }
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (quoted) {
            if (ch == '\\' && index + 1 < value.size()) {
                ++index;
            } else if (ch == '"') {
                quoted = false;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == delimiter && index >= start) {
            return index;
        }
    }
    return value.size();
}

namespace detail {

// Splits one OWS-trimmed `name=value` segment on its first '=' and hands the
// OWS-trimmed pair to the visitor. A segment without '=' is not a parameter and
// is skipped. Returns the visitor's keep-going result.
template <typename visitor_type>
[[nodiscard]] inline bool http_emit_field_parameter(std::string_view part, visitor_type& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
}

}  // namespace detail

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Members are OWS-trimmed and empty members
// are reported; framing-sensitive callers can reject them. Returning false from
// the visitor stops traversal. Views borrow the input field.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ',');
        if (!visitor(http_trim_ows(value.substr(start, end - start))) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Values keep their
// quotes. Do not use for Cookie: RFC 6265 gives '"' no special meaning there.
// Views borrow the input.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!detail::http_emit_field_parameter(http_trim_ows(value.substr(start, end - start)), visitor) ||
            end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] constexpr std::string_view http_trim_quoted_field_value(borrowed_text text) noexcept {
    auto value = text.view();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

// Allocation-free RFC 9110 section 5.6.4 quoted-string reader. The input starts
// at the opening DQUOTE; next() validates and decodes one content byte at a time
// (qdtext or the byte of a quoted-pair: HTAB, SP, VCHAR, obs-text) and returns
// false at the closing DQUOTE or on malformed input. closed() tells the two
// apart; consumed() then counts the bytes read, both quotes included. Bytes after
// the closing DQUOTE are never read.
class http_quoted_string_cursor final {
public:
    explicit constexpr http_quoted_string_cursor(borrowed_text input) noexcept
        : input_(input.view()),
          state_(!input_.empty() && input_.front() == '"' ? state_type::open : state_type::failed) {}

    [[nodiscard]] constexpr bool next(unsigned char& out) noexcept {
        if (state_ != state_type::open) {
            return false;
        }
        if (position_ == input_.size()) {
            state_ = state_type::failed;
            return false;
        }
        auto byte = static_cast<unsigned char>(input_[position_++]);
        if (byte == '"') {
            state_ = state_type::closed;
            return false;
        }
        if (byte == '\\') {
            if (position_ == input_.size()) {
                state_ = state_type::failed;
                return false;
            }
            byte = static_cast<unsigned char>(input_[position_++]);
        }
        if (!is_http_field_value_char(byte)) {
            state_ = state_type::failed;
            return false;
        }
        out = byte;
        return true;
    }

    [[nodiscard]] constexpr bool closed() const noexcept {
        return state_ == state_type::closed;
    }

    [[nodiscard]] constexpr std::size_t consumed() const noexcept {
        return position_;
    }

private:
    enum class state_type : unsigned char { open,
        closed,
        failed };

    std::string_view input_;
    std::size_t position_{1};
    state_type state_;
};

// Byte length of the well-formed quoted-string that starts `input`, quotes
// included, or 0 when `input` does not start with one.
[[nodiscard]] constexpr std::size_t http_quoted_string_size(std::string_view input) noexcept {
    http_quoted_string_cursor cursor(input);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
    }
    return cursor.closed() ? cursor.consumed() : 0;
}

// True when the whole of `value` is exactly one well-formed quoted-string.
[[nodiscard]] constexpr bool http_is_quoted_string(std::string_view value) noexcept {
    return !value.empty() && http_quoted_string_size(value) == value.size();
}

// Iterates a comma-delimited HTTP field-name list without allocating. Empty
// members are ignored, including an entirely empty list. Each returned view
// borrows the input. Drain next() until it returns nullopt, then check valid()
// to distinguish end from malformed input.
class http_field_name_list final {
public:
    explicit http_field_name_list(borrowed_text field) noexcept
        : remaining_(field.view()) {}

    [[nodiscard]] std::optional<std::string_view> next() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return finished_ && valid_;
    }

private:
    std::string_view remaining_;
    bool finished_{false};
    bool valid_{false};
};

}  // namespace ruvia

namespace ruvia::detail {

// RFC 9110 section 5.6.1 deliberately gives senders and recipients different
// list obligations: senders cannot generate empty members, while recipients
// must ignore a reasonable number of them. The HTTP/1 head-size limit bounds
// recipient work, so the tolerant path remains allocation-free and O(head).
enum class http_field_list_role : std::uint8_t { recipient,
    sender };

enum class http_field_list_parse_status : std::uint8_t { ok,
    malformed,
    rejected };

// The one role-aware list walk (RFC 9110 section 5.6.1) over the quote-aware
// splitter: an empty member is malformed for a sender and skipped for a
// recipient; every other member goes to `visitor`, whose non-ok status stops
// the walk and is returned.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_list_members(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    auto status = http_field_list_parse_status::ok;
    http_visit_comma_separated_quoted_field_items(value, [&status, role, &visitor](std::string_view member) {
        if (member.empty()) {
            if (role == http_field_list_role::sender) {
                status = http_field_list_parse_status::malformed;
            }
        } else {
            status = visitor(member);
        }
        return status == http_field_list_parse_status::ok;
    });
    return status;
}

// `#token` lists: a member that is not a token is malformed; a visitor that
// returns false rejects the field.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_token_list(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_list_members(value, role, [&visitor](std::string_view member) {
        if (!is_valid_http_header_name(member)) {
            return http_field_list_parse_status::malformed;
        }
        return visitor(member) ? http_field_list_parse_status::ok : http_field_list_parse_status::rejected;
    });
}

// The first nonempty list member accepted by `predicate`, or an empty view.
// Members are not validated; a quoted-string member is never split.
template <typename predicate_type>
[[nodiscard]] inline std::string_view http_find_header_token(
    std::string_view value, predicate_type&& predicate) noexcept {
    std::string_view found;
    http_visit_comma_separated_quoted_field_items(value, [&found, &predicate](std::string_view member) noexcept {
        if (!member.empty() && predicate(member)) {
            found = member;
            return false;
        }
        return true;
    });
    return found;
}

template <http_temporary_owning_char_string value_type, typename predicate_type>
std::string_view http_find_header_token(value_type&&, predicate_type&&) = delete;

// Accept-style negotiation fields skip empty list items without a role rule;
// grammar-checked lists use http_visit_list_members.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted(std::string_view value, visitor_type&& visitor) {
    http_visit_comma_separated_quoted_field_items(
        value, [&visitor](std::string_view item) { return item.empty() || visitor(item); });
}

// Plain comma-delimited token scan for fields whose grammar never permits
// quoted-string (such as Connection and Vary). Empty segments are skipped.
template <typename visitor_type>
inline void http_visit_comma_separated_tokens(std::string_view value, visitor_type&& visitor) {
    for (;;) {
        const auto comma = value.find(',');
        const auto item = http_trim_ows(value.substr(0, comma));
        if (!item.empty() && !visitor(item)) {
            return;
        }
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

// True when `predicate` accepts every ';'-separated parameter that follows a
// field value's leading token — the media type, coding or extension name, which
// carries no '=' and is never a parameter itself. The scan is quote-aware, so a
// ';' inside a quoted parameter value does not split, and each parameter reaches
// the predicate with its surrounding whitespace trimmed. A value with no ';' has
// no parameters and trivially satisfies the predicate.
//
// Unlike http_visit_semicolon_parameters_quoted_field this reports rejection: a predicate
// that returns false stops the walk and fails the whole field value. Use it
// where a malformed parameter must invalidate the field rather than be skipped.
template <typename predicate_type>
[[nodiscard]] inline bool http_all_parameters(std::string_view value, predicate_type&& predicate) {
    auto start = http_find_unquoted_delimiter(value, 0, ';');
    if (start >= value.size()) {
        return true;
    }
    ++start;
    while (start <= value.size()) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!predicate(http_trim_ows(value.substr(start, end - start)))) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline bool http_has_token(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    const auto expected_first = http_ascii_to_lower(static_cast<unsigned char>(expected.front()));
    return !http_find_header_token(value, [expected, expected_first](std::string_view token) noexcept {
        return token.size() == expected.size() &&
               http_ascii_to_lower(static_cast<unsigned char>(token.front())) == expected_first &&
               http_ascii_equals_ignore_case(token, expected);
    }).empty();
}

[[nodiscard]] inline bool is_http_token_or_quoted_string(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return value.front() == '"' ? http_is_quoted_string(value) : is_valid_http_header_name(value);
}

// RFC 9112 section 7 transfer-parameter: `token BWS "=" BWS ( token / quoted-string )`.
// `part` is one OWS-trimmed segment as produced by http_all_parameters.
[[nodiscard]] inline bool is_valid_http_transfer_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(http_trim_ows(part.substr(0, equals))) &&
           is_http_token_or_quoted_string(http_trim_ows(part.substr(equals + 1)));
}

[[nodiscard]] inline std::string_view http_header_token_before_parameters(
    std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

template <http_temporary_owning_char_string value_type>
std::string_view http_header_token_before_parameters(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia {

// Protocol version is HTTP message control data, not a borrowed wire token.
// HTTP/1 carries it in the start-line; HTTP/2 and HTTP/3 establish it from
// connection framing even though no textual version appears in a field block.
enum class http_protocol_version : std::uint8_t {
    http10,
    http11,
    http2,
    http3,
};

enum class http_scheme : std::uint8_t {
    http,
    https,
};

enum class http_known_method : std::uint8_t {
    get,
    post,
    put,
    delete_value,
    patch,
    head,
    options,
    connect,
    unknown
};

[[nodiscard]] constexpr std::uint32_t http_known_method_bit(http_known_method method) noexcept {
    return method == http_known_method::unknown
               ? 0U
               : (std::uint32_t{1} << static_cast<std::uint32_t>(method));
}

// HTTP methods are an extensible, case-sensitive token space. http_known_method is
// only the framework's fixed semantic classification; it is never the wire value.
[[nodiscard]] http_known_method classify_http_method(std::string_view method) noexcept;
[[nodiscard]] std::string_view known_http_method_token(http_known_method method) noexcept;
[[nodiscard]] bool is_valid_http_method_token(std::string_view method) noexcept;

// RFC 9110 method properties over the exact, case-sensitive wire token. Unknown
// extension methods are conservatively neither safe nor idempotent; TRACE is
// recognized even though it is intentionally not a routable fixed enum member.
[[nodiscard]] bool is_http_method_safe(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_idempotent(std::string_view method) noexcept;

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class http_header final {
public:
    http_header(std::string_view name, std::string_view value)
        : name_size_(name.size()),
          value_size_(value.size()),
          owned_(true) {
        if (value.size() > (std::numeric_limits<std::size_t>::max)() - name.size()) {
            throw std::length_error("HTTP header storage size overflows size_t");
        }
        const auto size = name.size() + value.size();
        if (size != 0) {
            bytes_ = new char[size];
            if (!name.empty()) {
                std::memcpy(bytes_, name.data(), name.size());
            }
            if (!value.empty()) {
                std::memcpy(bytes_ + name.size(), value.data(), value.size());
            }
        }
    }

    http_header(const http_header& other)
        : bytes_(!other.owned_ ? other.bytes_ : nullptr),
          name_size_(other.name_size_),
          value_size_(other.value_size_),
          owned_(other.owned_) {
        if (owned_) {
            const auto size = name_size_ + value_size_;
            if (size != 0) {
                bytes_ = new char[size];
                std::memcpy(bytes_, other.bytes_, size);
            }
        }
    }

    http_header(http_header&& other) noexcept
        : bytes_(std::exchange(other.bytes_, nullptr)),
          name_size_(std::exchange(other.name_size_, 0)),
          value_size_(std::exchange(other.value_size_, 0)),
          owned_(std::exchange(other.owned_, false)) {}

    http_header& operator=(const http_header& other) {
        if (this != &other) {
            http_header replacement(other);
            swap_storage(replacement);
        }
        return *this;
    }

    http_header& operator=(http_header&& other) noexcept {
        if (this != &other) {
            http_header retired(std::move(*this));
            bytes_ = std::exchange(other.bytes_, nullptr);
            name_size_ = std::exchange(other.name_size_, 0);
            value_size_ = std::exchange(other.value_size_, 0);
            owned_ = std::exchange(other.owned_, false);
        }
        return *this;
    }

    ~http_header() noexcept {
        release();
    }

    // Owning boundaries that normalize field names lower-case them while
    // copying, so the field still costs one allocation.
    [[nodiscard]] static http_header copy_with_lowercase_name(
        std::string_view name, std::string_view value) {
        auto result = http_header(name, value);
        for (std::size_t index = 0; index < result.name_size_; ++index) {
            result.bytes_[index] =
                static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(result.bytes_[index])));
        }
        return result;
    }

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return name_size_ != 0 ? std::string_view(bytes_, name_size_) : std::string_view{};
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_size_ != 0 ? std::string_view(bytes_ + name_size_, value_size_) : std::string_view{};
    }
    std::string_view value() const&& = delete;

private:
    friend class websocket_client_negotiation;

    constexpr http_header() noexcept = default;

    constexpr http_header(
        const char* static_bytes, std::size_t name_size, std::size_t value_size) noexcept
        : bytes_(const_cast<char*>(static_bytes)),
          name_size_(name_size),
          value_size_(value_size),
          owned_(false) {}

    void release() noexcept {
        auto* const bytes = std::exchange(bytes_, nullptr);
        name_size_ = 0;
        value_size_ = 0;
        if (std::exchange(owned_, false) && bytes != nullptr) {
            delete[] bytes;
        }
    }

    void swap_storage(http_header& other) noexcept {
        std::swap(bytes_, other.bytes_);
        std::swap(name_size_, other.name_size_);
        std::swap(value_size_, other.value_size_);
        std::swap(owned_, other.owned_);
    }

    char* bytes_{nullptr};
    std::size_t name_size_{};
    std::size_t value_size_{};
    bool owned_{false};
};

class http_header_view final {
public:
    constexpr http_header_view() noexcept = default;

    constexpr http_header_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    http_header_view(name_type&& name, value_type&& value) = delete;

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

// Zero-copy HTTP header-view span held by a request, response, handshake, or
// client option struct. Like borrowed_text, it accepts borrowed spans and
// contiguous owning-container lvalues while rejecting temporary owning
// containers (std::vector, std::array) and initializer_list
// values whose backing storage would expire before the struct is consumed.
using borrowed_http_headers = borrowed_span<http_header_view>;

static_assert(sizeof(borrowed_http_headers) == sizeof(std::span<const http_header_view>));

template <typename field_type>
concept http_header_field = requires(const std::remove_reference_t<field_type>& field) {
    { field.name() } -> std::convertible_to<std::string_view>;
    { field.value() } -> std::convertible_to<std::string_view>;
};

template <typename range_type>
concept http_header_field_range =
    std::ranges::contiguous_range<const std::remove_cvref_t<range_type>> &&
    http_header_field<std::ranges::range_reference_t<const std::remove_cvref_t<range_type>>>;

namespace detail {
class http_header_range_view;
}  // namespace detail

template <typename source_type>
concept http_header_field_source =
    std::same_as<std::remove_cvref_t<source_type>, detail::http_header_range_view> ||
    http_header_field_range<source_type> ||
    requires(const std::remove_cvref_t<source_type>& source) {
        { source.headers() } -> http_header_field_range;
    };
namespace detail {

class http_header_range_view final {
public:
    using accessor_type = http_header_view (*)(const void*, std::size_t) noexcept;

    constexpr http_header_range_view() noexcept = default;

    template <http_header_field_source source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, http_header_range_view>)
    constexpr http_header_range_view(const source_type& source) noexcept {
        if constexpr (http_header_field_range<source_type>) {
            using value_type = std::remove_cv_t<std::ranges::range_value_t<const source_type>>;
            data_ = std::ranges::data(source);
            size_ = std::ranges::size(source);
            at_ = +[](const void* base, std::size_t index) noexcept -> http_header_view {
                const auto& field = static_cast<const value_type*>(base)[index];
                return {field.name(), field.value()};
            };
        } else {
            *this = http_header_range_view(source.headers());
        }
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr http_header_view operator[](std::size_t index) const noexcept {
        return at_(data_, index);
    }

    class iterator final {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = http_header_view;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;
        constexpr iterator(const void* data, accessor_type at, std::size_t index) noexcept
            : data_(data),
              at_(at),
              index_(index) {}

        [[nodiscard]] constexpr http_header_view operator*() const noexcept {
            return at_(data_, index_);
        }
        constexpr iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        constexpr iterator operator++(int) noexcept {
            auto copy = *this;
            ++index_;
            return copy;
        }
        [[nodiscard]] friend constexpr bool operator==(
            const iterator& left, const iterator& right) noexcept {
            return left.data_ == right.data_ && left.index_ == right.index_;
        }

    private:
        const void* data_{nullptr};
        accessor_type at_{nullptr};
        std::size_t index_{0};
    };

    [[nodiscard]] constexpr iterator begin() const noexcept {
        return {data_, at_, 0};
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return {data_, at_, size_};
    }

private:
    const void* data_{nullptr};
    std::size_t size_{0};
    accessor_type at_{nullptr};
};
}  // namespace detail

// RFC 9218 section 4.1: urgency is 0..7, defaulting to 3. Local numeric
// inputs are checked before narrowing. Peer fields instead use the tolerant
// parser below, which ignores out-of-range values as required by section 4.
class http_priority_urgency final {
public:
    constexpr http_priority_urgency() noexcept = default;

    template <std::integral integer_type>
    constexpr http_priority_urgency(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint8_t>(value)
                     : throw std::invalid_argument("HTTP priority urgency must be between 0 and 7")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::cmp_less_equal(+value, 7);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_priority_urgency> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_priority_urgency(static_cast<std::uint8_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint8_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_priority_urgency, http_priority_urgency) = default;

private:
    struct validated_tag_type {};

    constexpr http_priority_urgency(std::uint8_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint8_t value_{3};
};

static_assert(std::is_trivially_copyable_v<http_priority_urgency>);
static_assert(sizeof(http_priority_urgency) == sizeof(std::uint8_t));

struct http_priority final {
    http_priority_urgency urgency_{};
    bool incremental_{false};
};
struct http_priority_fields final {
    std::optional<http_priority_urgency> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept {
        return {urgency_.value_or(http_priority_urgency{}), incremental_.value_or(false)};
    }
};
enum class http_priority_error : std::uint8_t { invalid_syntax,
    invalid_value,
    invalid_frame,
    output_too_small };
struct http_priority_update final {
    std::uint64_t element_id_{0};
    bool push_{false};
    http_priority_fields fields_{};
};

// RFC 9218 / RFC 8941 Dictionary parsing. Unknown members, invalid parameter
// types and out-of-range values are ignored; malformed structured syntax fails.
// Missing response parameters remain absent for intermediary merging.
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept;
namespace detail {
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept;
}  // namespace detail

// Reads all Priority field lines in wire order without allocating. A later
// dictionary member replaces the earlier member, including an invalid value.
template <http_header_field_source headers_type>
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(
    const headers_type& headers) noexcept {
    return detail::parse_http_priority_headers(headers);
}
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t stream_id, http_priority_fields fields_value) noexcept;

struct http_push_request_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text path_{"/"};
    borrowed_http_headers headers_{};
};

// Detached promise metadata owns its decoded bytes independently of feed input.
// Accepting a promise does
// not establish transport authority: the driver validates the URI origin using
// its authenticated connection before consuming the pushed response.
struct http_push_request final {
    http_push_request() noexcept = default;
    http_push_request(const http_push_request&) = delete;
    http_push_request& operator=(const http_push_request&) = delete;
    http_push_request(http_push_request&&) noexcept = default;
    http_push_request& operator=(http_push_request&&) = delete;
    std::string method_;
    std::string scheme_;
    std::string authority_;
    std::string path_;
    std::vector<http_header> headers_;
};

struct http2_push_promise_event final {
    std::uint32_t associated_stream_id_;
    std::uint32_t promised_stream_id_;
    http_push_request request_;
};

enum class http2_push_submit_error : std::uint8_t {
    invalid_state,
    push_disabled,
    stream_limit,
    invalid_request,
};

// Encoders validate limits before allocation and write into the final owned result.
struct http_origin_advertisement final {
    std::vector<std::string> origins_;
    http_origin_advertisement() noexcept = default;
    http_origin_advertisement(const http_origin_advertisement&) = default;
    http_origin_advertisement(http_origin_advertisement&&) noexcept = default;
    http_origin_advertisement& operator=(const http_origin_advertisement&) = default;
    http_origin_advertisement& operator=(http_origin_advertisement&&) noexcept = default;
};
struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::string origin_;
    std::string field_value_;
    http_alternative_service_advertisement() noexcept = default;
    http_alternative_service_advertisement(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement(http_alternative_service_advertisement&&) noexcept = default;
    http_alternative_service_advertisement& operator=(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement& operator=(http_alternative_service_advertisement&&) noexcept = default;
};
enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
};
// RFC 8336 ORIGIN payload decoding. Invalid serialized
// origins are ignored; truncated length-delimited entries reject the payload.
// Origin authority and connection coalescing require the runtime's TLS identity.
[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error>
decode_http_origin_advertisement(std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t max_frame_size = 16384);
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error>
decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value,
    std::uint32_t max_frame_size = 16384);

// Outbound Content-Length in 0..UINT64_MAX. Checked before narrowing so
// negative inputs cannot wrap into huge finite lengths.
class http_content_length final {
public:
    constexpr http_content_length() noexcept = default;

    template <std::integral integer_type>
    constexpr http_content_length(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint64_t>(value)
                     : throw std::invalid_argument("HTTP Content-Length must be in 0..18446744073709551615")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_content_length> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_content_length(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_content_length, http_content_length) = default;

private:
    struct validated_tag_type {};

    constexpr http_content_length(std::uint64_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint64_t value_{0};
};

static_assert(std::is_trivially_copyable_v<http_content_length>);
static_assert(sizeof(http_content_length) == sizeof(std::uint64_t));

// RFC 9110 sender requirements when a request carries content. Explicit
// zero-length framing does not create content; recipients frame it separately.
enum class http_request_content_semantics : std::uint8_t {
    no_additional_requirements,
    forbidden,
    content_type_required,
};

[[nodiscard]] constexpr http_request_content_semantics classify_http_request_content_semantics(
    std::string_view method) noexcept {
    if (method == "CONNECT" || method == "TRACE") {
        return http_request_content_semantics::forbidden;
    }
    if (method == "OPTIONS") {
        return http_request_content_semantics::content_type_required;
    }
    return http_request_content_semantics::no_additional_requirements;
}

// Whether a request method should explicitly send Content-Length: 0 when
// forwarded or generated with no body (false for GET, HEAD, DELETE, OPTIONS,
// TRACE, CONNECT; true for POST, PUT, PATCH and extension methods).
[[nodiscard]] constexpr bool http_method_sends_zero_content_length(
    std::string_view method) noexcept {
    if (method.empty() || method == "GET" || method == "HEAD" || method == "DELETE" ||
        method == "OPTIONS" || method == "TRACE" || method == "CONNECT") {
        return false;
    }
    return true;
}

// One outbound content contract for an HTTP/2 or HTTP/3 request head: it
// determines both the generated Content-Length and whether the head ends the
// request stream. Public callers and both protocol engines use this value.
class http_request_content_plan;

class http_request_without_content final {
private:
    friend class http_request_content_plan;
    constexpr http_request_without_content() noexcept = default;
};

class http_known_length_request_content final {
public:
    [[nodiscard]] constexpr http_content_length length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_known_length_request_content(http_content_length length) noexcept
        : length_(length) {}
    http_content_length length_;
};

class http_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<http_content_length> expected_length() const noexcept {
        return length_;
    }

private:
    friend class http_request_content_plan;
    explicit constexpr http_streaming_request_content(std::optional<http_content_length> length) noexcept
        : length_(length) {}
    std::optional<http_content_length> length_{};
};

class http_request_content_plan final {
public:
    [[nodiscard]] static constexpr http_request_content_plan none() noexcept {
        return http_request_content_plan(http_request_without_content());
    }
    // Zero describes no content: it does not require OPTIONS Content-Type and
    // cannot use Expect: 100-continue. TRACE omits the zero Content-Length.
    // The content is submitted after the head, so the head never ends the
    // stream unless the length is zero.
    [[nodiscard]] static constexpr http_request_content_plan known_length(http_content_length length) noexcept {
        return http_request_content_plan(http_known_length_request_content(length));
    }
    // A known zero keeps the send half open until the request is finished; it
    // cannot send content or wait for 100 Continue.
    [[nodiscard]] static constexpr http_request_content_plan streaming(
        std::optional<http_content_length> length = {}) noexcept {
        return http_request_content_plan(http_streaming_request_content(length));
    }
    [[nodiscard]] constexpr const http_request_without_content* without_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_request_without_content>(&value_);
    }
    const http_request_without_content* without_content() const&& = delete;
    [[nodiscard]] constexpr const http_known_length_request_content* known_length_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_known_length_request_content>(&value_);
    }
    const http_known_length_request_content* known_length_content() const&& = delete;
    [[nodiscard]] constexpr const http_streaming_request_content* streaming_content() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_streaming_request_content>(&value_);
    }
    const http_streaming_request_content* streaming_content() const&& = delete;
    // The Content-Length the head declares, if any.
    [[nodiscard]] constexpr std::optional<http_content_length> declared_length() const noexcept {
        if (const auto* known = std::get_if<http_known_length_request_content>(&value_)) {
            return known->length();
        }
        if (const auto* streaming = std::get_if<http_streaming_request_content>(&value_)) {
            return streaming->expected_length();
        }
        return std::nullopt;
    }

private:
    using value_type = std::variant<http_request_without_content, http_known_length_request_content,
        http_streaming_request_content>;
    explicit constexpr http_request_content_plan(http_request_without_content value) noexcept
        : value_(value) {}
    explicit constexpr http_request_content_plan(http_known_length_request_content value) noexcept
        : value_(value) {}
    explicit constexpr http_request_content_plan(http_streaming_request_content value) noexcept
        : value_(value) {}
    value_type value_;
};

// Request fields the HTTP protocol layer generates or interprets itself when it
// frames and transports a request: Host (RFC 9110 section 7.2), message framing
// (Content-Length, Transfer-Encoding, Trailer; RFC 9110 sections 6.5 and 8.6),
// Expect (RFC 9110 section 10.1.1), and connection management (Connection and
// the connection-specific Keep-Alive, Proxy-Connection, TE and Upgrade; RFC
// 9110 section 7.6.1). A component that builds a request on an application's
// behalf owns these fields and must not accept them from the application.
// Field names are matched case-insensitively.
[[nodiscard]] bool is_http_protocol_managed_request_field(std::string_view name) noexcept;

// 校验应用提供的单个出站请求首部，不分配内存。字段名须为 token，
// 字段值须符合 RFC 9110 §5.5 的规范化语法（不接受首尾 SP/HTAB），
// 并通过请求 writer 的已知字段发送方语义校验；未知合法扩展允许空值。
// 拒绝上面的协议托管字段，名称按大小写不敏感匹配；因此无需 HTTP 版本参数。
// 这不是响应首部或接收方校验器。调用方负责字段间重复及完整消息约束；
// 返回 true 不代表整个请求可发送，不保留输入或跨调用状态。
[[nodiscard]] bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept;

class http_response_headers;

// Bytes a field costs beyond its name and value: the ": " separator and the
// CRLF of its normalized HTTP/1 field line "name: value\r\n".
inline constexpr std::size_t http_field_line_overhead = 4;

// The byte and count rule for an HTTP field section, for headers and trailers,
// requests and responses, received and generated: the section must fit
// max_http_header_bytes, each field costing name.size() + value.size() +
// http_field_line_overhead (independent of received optional whitespace), and
// hold at most max_http_header_fields fields.
// HTTP/2 header lists are the deliberate exception: they are charged by the
// RFC 9113 section 6.5.2 SETTINGS_MAX_HEADER_LIST_SIZE accounting (name and
// value plus 32 bytes per field, pseudo-fields included) against the same limit.
// Fixed head bytes such as a start line or the terminating empty line are
// charged with add(); a trailer section starts from
// http_trailer_section_budget(). A failed charge leaves the budget unchanged.
class http_field_section_budget final {
public:
    [[nodiscard]] constexpr bool add(std::size_t bytes_value) noexcept {
        if (bytes_value > max_http_header_bytes - bytes_) {
            return false;
        }
        bytes_ += bytes_value;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::size_t name_bytes, std::size_t value_bytes) noexcept {
        const auto remaining = max_http_header_bytes - bytes_;
        if (fields_ == max_http_header_fields || remaining < http_field_line_overhead ||
            name_bytes > remaining - http_field_line_overhead ||
            value_bytes > remaining - http_field_line_overhead - name_bytes) {
            return false;
        }
        bytes_ += name_bytes + value_bytes + http_field_line_overhead;
        ++fields_;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::string_view name, std::string_view value) noexcept {
        return add_field(name.size(), value.size());
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
    std::size_t fields_{0};
};

// The budget of a trailer section before its first field. Like a head, the
// section is charged its terminating empty line, so every section a writer
// generates also fits the decoders' limit on the complete encoded section.
[[nodiscard]] constexpr http_field_section_budget http_trailer_section_budget() noexcept {
    http_field_section_budget budget;
    (void)budget.add(std::string_view("\r\n").size());
    return budget;
}

namespace detail {

[[nodiscard]] constexpr std::size_t saturating_add_size(
    std::size_t left, std::size_t right) noexcept {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    return left > maximum - right ? maximum : left + right;
}

template <typename field_span_type>
[[nodiscard]] constexpr std::size_t estimate_http1_fields_bytes_impl(
    const field_span_type& fields, bool include_terminating_crlf) noexcept {
    std::size_t total = include_terminating_crlf ? 2U : 0U;
    for (const auto& field : fields) {
        total = saturating_add_size(total, field.name().size());
        total = saturating_add_size(total, field.value().size());
        total = saturating_add_size(total, http_field_line_overhead);
    }
    return total;
}

}  // namespace detail

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header_view> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] std::size_t estimate_http1_field_section_bytes(
    const http_response_headers& fields, bool include_terminating_crlf = true) noexcept;

// Equivalent HTTP/1.1 wire byte size of a response status line ("HTTP/1.1 NNN
// <reason>\r\n"), all header field lines, and the terminating CRLF.
[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header_view> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] std::size_t estimate_http1_response_head_bytes(
    http_status_code status, const http_response_headers& headers) noexcept;

namespace detail {
// Repeated fields retain their wire order; singular convenience accessors
// select the last matching field without copying its borrowed value.
template <typename field_type>
[[nodiscard]] std::optional<std::string_view> find_last_http_header(
    std::span<const field_type> fields, std::string_view name) noexcept {
    for (auto field = fields.rbegin(); field != fields.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace detail

}  // namespace ruvia

#pragma once

namespace ruvia {
enum class http_request_trailer_error : unsigned char { invalid_field,
    forbidden_field,
    section_too_large };

// Owns decoded terminal request fields, separately from the initial head.
// Drivers populate this object before reporting successful body completion.
// The section must fit the http_field_section_budget byte and field-count rule,
// starting from http_trailer_section_budget(). Retained field names are
// lowercase whichever append overload or protocol supplied them.
class http_request_trailers final {
public:
    http_request_trailers() noexcept = default;
    http_request_trailers(const http_request_trailers&) = delete;
    http_request_trailers& operator=(const http_request_trailers&) = delete;
    // Appends one field or a span of fields all-or-nothing. A name not permitted
    // in request trailers (RFC 9110 section 6.5.1) reports forbidden_field.
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header_view> fields);
    template <detail::http_temporary_owning_range<http_header_view> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    // Appends a received trailer section all-or-nothing, as decoded by any
    // protocol version (http1_chunked_body_decoder, HTTP/2 or HTTP/3 message
    // end events). Every field must be a valid regular field with a lowercase
    // name; a pseudo-header field makes the section malformed (RFC 9113
    // section 8.1, RFC 9114 section 4.3) and reports invalid_field. Every
    // received field is charged to the byte budget; fields not permitted in
    // request trailers are discarded rather than retained (RFC 9110 section
    // 6.5.1, RFC 9112 section 7.1.2).
    [[nodiscard]] std::optional<http_request_trailer_error> append_received(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append_received(fields_type&&) = delete;
    [[nodiscard]] std::vector<http_header> take_fields() && noexcept {
        budget_ = http_trailer_section_budget();
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const http_header> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_;
    }
    std::span<const http_header> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    std::vector<http_header> fields_;
    http_field_section_budget budget_{http_trailer_section_budget()};
};
}  // namespace ruvia

namespace ruvia::detail {

enum class http_field_kind : std::uint8_t;

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept;

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept;

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

// The single rule for one generated request trailer field: valid field syntax
// (RFC 9110 section 5), a name a sender may place in request trailers (RFC 9110
// section 6.5.1), and a charge to the section `budget`, which is unchanged on
// failure.
[[nodiscard]] inline std::optional<http_request_trailer_error> charge_http_request_trailer_field(
    http_field_section_budget& budget, std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return http_request_trailer_error::invalid_field;
    }
    if (is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    if (!budget.add_field(name, value)) {
        return http_request_trailer_error::section_too_large;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

namespace ruvia {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 combines its receive-half and remaining-content states so an open
// metadata-only or known-empty stream cannot masquerade as pending content.
enum class http_request_content_indication : std::uint8_t { no_content,
    will_follow };

// RFC 9110 Section 10.1.1 forbids a client from generating 100-continue when
// the request has no content. Keep this sender check next to the recipient-side
// expectation state so HTTP/1 and HTTP/2 cannot derive different answers.
[[nodiscard]] constexpr bool http_client_expectation_is_valid(
    bool has_continue, http_request_content_indication content) noexcept {
    return !has_continue || content == http_request_content_indication::will_follow;
}

// Whether the product accepts unknown expectation extensions. Expect remains
// valid syntax either way; the HTTP contract owns the protocol response chosen
// by the explicit rejection policy.
enum class http_unsupported_expectation_policy : std::uint8_t { ignore,
    reject };

enum class http_server_expectation_plan : std::uint8_t {
    no_action,
    send_continue,
    reject_unsupported,
};

[[nodiscard]] inline http_protocol_error http_unsupported_expectation_error() noexcept {
    return http_protocol_error(
        http_status::expectation_failed, "unsupported_expectation", "unsupported Expect header");
}

// Incremental recipient-side state for the RFC 9110 Expect #list. Repeated field
// lines extend the same logical list, empty members are ignored, and all state is
// fixed-size. The only standardized member is 100-continue; every other non-empty
// member is retained as the single semantic fact "unsupported" for product policy.
class http_request_expectations final {
public:
    void parse_field(std::string_view value) noexcept;

    [[nodiscard]] bool has_continue() const noexcept {
        return (flags_ & continue_value) != 0;
    }

    [[nodiscard]] bool has_unsupported() const noexcept {
        return (flags_ & unsupported) != 0;
    }

    // RFC 9110 requires an HTTP/1.0 recipient to ignore 100-continue. Preserve
    // the independent unsupported-member fact so the Web product can still apply
    // its chosen extension-support policy.
    void ignore_continue() noexcept {
        flags_ &= static_cast<std::uint8_t>(~continue_value);
    }

    [[nodiscard]] http_server_expectation_plan server_plan(http_request_content_indication content,
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        if (has_unsupported() && unsupported_policy == http_unsupported_expectation_policy::reject) {
            return http_server_expectation_plan::reject_unsupported;
        }
        if (has_continue() && content == http_request_content_indication::will_follow) {
            return http_server_expectation_plan::send_continue;
        }
        return http_server_expectation_plan::no_action;
    }

    // RFC 9110 section 7.8: a server that receives both Upgrade and an Expect
    // field with 100-continue MUST send 100 (Continue) before 101 (Switching
    // Protocols), even when no content follows the head. Unsupported members
    // are planned by server_plan() before the upgrade is accepted.
    [[nodiscard]] bool continue_before_switching_protocols() const noexcept {
        return has_continue();
    }

private:
    static constexpr std::uint8_t continue_value = 1U << 0;
    static constexpr std::uint8_t unsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<http_request_expectations>);
static_assert(sizeof(http_request_expectations) <= 1);
static_assert(std::is_trivially_copyable_v<http_server_expectation_plan>);
static_assert(sizeof(http_server_expectation_plan) <= 2);

class http_request;

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

namespace detail {
struct http_authority_view_access;
}

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

// A validated, borrowed RFC 3986 authority without userinfo; IP-literal hosts
// keep their brackets. An explicit empty port stays distinct from an absent one
// for syntax-preserving parsing; both map to the scheme default when an HTTP
// origin is compared.
class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept {
        return port_kind_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept {
        return port_kind_ == http_authority_port_kind::value ? std::optional<std::uint16_t>(port_)
                                                             : std::nullopt;
    }

    // Returns the explicit port or caller fallback. This numeric observation
    // alone does not preserve the absent/empty versus explicit-zero distinction.
    [[nodiscard]] constexpr std::uint16_t effective_port(port_number default_port) const noexcept {
        return port_kind_ == http_authority_port_kind::value ? port_ : default_port.value();
    }

private:
    friend struct detail::http_authority_view_access;

    constexpr http_authority_view(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port) noexcept
        : host_(host),
          port_(port),
          port_kind_(port_kind) {}

    std::string_view host_;
    std::uint16_t port_{0};
    http_authority_port_kind port_kind_{http_authority_port_kind::absent};
};

[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port, checked before narrowing like other port inputs.
// Zero means no known scheme default: absent/empty ports then compare equal
// only to absent/empty ports, never to an explicitly supplied zero.
// Invalid authorities never compare equal.
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, port_number default_port) noexcept;

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;

// Normalizes an incoming request target for proxy forwarding: converts
// absolute-form targets to origin-form (with "/" when the path is empty and
// preserving any query), converts an OPTIONS target with an empty path and no
// query to "*", and preserves all other forms verbatim.
[[nodiscard]] std::string normalize_http_proxy_request_target(
    std::string_view method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    http_known_method method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    const http_request& request);

enum class http_max_forwards_status : std::uint8_t {
    absent,
    valid,
    invalid,
};

// Parsed Max-Forwards hop count (RFC 9110 section 7.6.2).
class http_max_forwards_result final {
public:
    [[nodiscard]] static constexpr http_max_forwards_result absent() noexcept {
        return http_max_forwards_result(http_max_forwards_status::absent, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result invalid() noexcept {
        return http_max_forwards_result(http_max_forwards_status::invalid, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result valid(std::uint64_t current) noexcept {
        return http_max_forwards_result(http_max_forwards_status::valid, current);
    }

    [[nodiscard]] constexpr http_max_forwards_status status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool is_absent() const noexcept {
        return status_ == http_max_forwards_status::absent;
    }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return status_ == http_max_forwards_status::valid;
    }
    [[nodiscard]] constexpr bool is_invalid() const noexcept {
        return status_ == http_max_forwards_status::invalid;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> current() const noexcept {
        return is_valid() ? std::optional<std::uint64_t>(current_) : std::nullopt;
    }
    [[nodiscard]] constexpr bool exhausted() const noexcept {
        return is_valid() && current_ == 0;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> decremented() const noexcept {
        return (is_valid() && current_ > 0) ? std::optional<std::uint64_t>(current_ - 1)
                                            : std::nullopt;
    }
    [[nodiscard]] std::string format_decremented() const;

private:
    constexpr http_max_forwards_result(
        http_max_forwards_status status, std::uint64_t current) noexcept
        : current_(current),
          status_(status) {}

    std::uint64_t current_{0};
    http_max_forwards_status status_{http_max_forwards_status::absent};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_max_forwards_value(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header_view> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    const http_request& request,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;

class http_request;

// The connection facts RFC 9110 §7.4 weighs against a request's target URI.
// The server runtime supplies them; the request alone cannot know them.
struct http_target_origin_connection final {
    // The request arrived over a TLS-secured connection.
    bool secured_{false};
    // The peer is a trusted gateway (a configured reverse proxy). It, not this
    // connection, vouches for the target's scheme and authority, so §7.4
    // exempts the request from the connection checks.
    bool trusted_gateway_{false};
};

enum class http_target_origin_action : std::uint8_t {
    // Nothing on this connection must be verified before processing.
    accept,
    // An "https" target on a secured connection: process it only when the
    // certificate this connection presented is valid for host().
    verify_certificate,
    // The connection cannot carry this target: answer 421 (Misdirected
    // Request, RFC 9110 §15.5.20).
    misdirected,
};

enum class http_target_host_kind : std::uint8_t {
    // A reg-name, without the trailing dot of an absolute domain name.
    dns_name,
    ipv4,
    ipv6,
};

// What an origin server must establish about one request's target origin.
// Meaningful host data only for verify_certificate.
class http_target_origin final {
public:
    static constexpr std::size_t max_dns_name_size = 253;

    [[nodiscard]] constexpr http_target_origin_action action() const noexcept {
        return action_;
    }

    [[nodiscard]] constexpr http_target_host_kind host_kind() const noexcept {
        return host_kind_;
    }

    // dns_name: the host text (percent-encoded unreserved octets decoded, a
    // single trailing dot stripped, letter case preserved). ipv4/ipv6: empty.
    [[nodiscard]] constexpr std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND {
        return decoded_host_size_ != 0 ? std::string_view(decoded_host_.data(), decoded_host_size_) : host_;
    }
    std::string_view host() const&& = delete;

    // ipv4/ipv6: the address in network byte order (4 or 16 bytes).
    // dns_name: empty.
    [[nodiscard]] std::span<const std::uint8_t> address() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span(address_.data(), address_size_);
    }
    std::span<const std::uint8_t> address() const&& = delete;

    [[nodiscard]] static constexpr http_target_origin accepted() noexcept {
        return http_target_origin(http_target_origin_action::accept);
    }

    [[nodiscard]] static constexpr http_target_origin misdirected() noexcept {
        return http_target_origin(http_target_origin_action::misdirected);
    }

    [[nodiscard]] static constexpr http_target_origin dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_ = host;
        return origin;
    }

    template <detail::http_temporary_owning_char_string host_type>
    static void dns_name(host_type&&) = delete;

    [[nodiscard]] static constexpr http_target_origin decoded_dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.decoded_host_size_ = static_cast<std::uint8_t>(host.size());
        for (std::size_t i = 0; i < host.size(); ++i) {
            origin.decoded_host_[i] = host[i];
        }
        return origin;
    }

    [[nodiscard]] static constexpr http_target_origin ip_address(
        http_target_host_kind kind, std::span<const std::uint8_t> address) noexcept {
        if ((kind == http_target_host_kind::ipv4 && address.size() != 4) ||
            (kind == http_target_host_kind::ipv6 && address.size() != 16) ||
            (kind != http_target_host_kind::ipv4 && kind != http_target_host_kind::ipv6)) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_kind_ = kind;
        origin.address_size_ = static_cast<std::uint8_t>(address.size());
        for (std::size_t i = 0; i < address.size(); ++i) {
            origin.address_[i] = address[i];
        }
        return origin;
    }

private:
    explicit constexpr http_target_origin(http_target_origin_action action) noexcept
        : action_(action) {
        if (std::is_constant_evaluated()) {
            decoded_host_.fill('\0');
            address_.fill(0);
        }
    }

    std::string_view host_;
    std::array<char, max_dns_name_size> decoded_host_;
    std::array<std::uint8_t, 16> address_;
    std::uint8_t decoded_host_size_{0};
    std::uint8_t address_size_{0};
    http_target_origin_action action_;
    http_target_host_kind host_kind_{http_target_host_kind::dns_name};
};

// RFC 9110 §7.4: unless the connection is from a trusted gateway, an origin
// server MUST reject a request whose target URI's scheme-specific requirements
// the connection does not meet; an "https" resource only over a connection
// secured by a certificate valid for the target's origin. Decides, for a
// parsed request of any HTTP version:
// - trusted gateway: accept;
// - an "https" target (absolute-form scheme or HTTP/2 :scheme, compared
//   case-insensitively) on a cleartext connection: misdirected;
// - a secured connection whose target scheme is "https" -- explicit, or
//   implied for origin-form and asterisk-form (RFC 9112 §3.3): verify the
//   target host (:authority, else Host) against the certificate. An
//   IPvFuture host, which no certificate names, is misdirected;
// - everything else accepts: "http" targets on either transport (RFC 9110
//   imposes no certificate requirement on them), CONNECT tunnel targets
//   (authority-form, or HTTP/2 CONNECT without :scheme), and targets with an
//   empty authority, which RFC 9112 §3.3 completes with the server's
//   configured default rather than a client-chosen name.
// The port is not compared: certificates name hosts, not ports. Allocation free.
[[nodiscard]] http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept;

enum class http_request_target_form : std::uint8_t {
    origin,
    absolute,
    authority,
    asterisk,
    // HTTP/2 and HTTP/3 carry target components as pseudo-fields rather
    // than one HTTP/1 request-target token.
    http2,
    http3,
};

namespace detail {

enum class http_field_kind : std::uint8_t;
struct http_request_access;

// Owns field descriptors and the parser's http_field_kind for each field.
// Kinds sit in the same allocation so Cookie/Accept scans do not re-classify
// names. Production parsers reserve the final size once after validating the
// head. Moving this block never allocates.
class http_request_header_block final {
public:
    http_request_header_block() noexcept = default;
    http_request_header_block(const http_request_header_block&) = delete;
    http_request_header_block& operator=(const http_request_header_block&) = delete;
    http_request_header_block(http_request_header_block&& other) noexcept
        : fields_(std::exchange(other.fields_, nullptr)),
          kinds_(std::exchange(other.kinds_, nullptr)),
          size_(std::exchange(other.size_, std::uint16_t{0})),
          capacity_(std::exchange(other.capacity_, std::uint16_t{0})),
          repeated_known_bits_(std::exchange(other.repeated_known_bits_, std::uint32_t{0})) {}
    http_request_header_block& operator=(http_request_header_block&& other) noexcept {
        if (this != &other) {
            release();
            fields_ = std::exchange(other.fields_, nullptr);
            kinds_ = std::exchange(other.kinds_, nullptr);
            size_ = std::exchange(other.size_, std::uint16_t{0});
            capacity_ = std::exchange(other.capacity_, std::uint16_t{0});
            repeated_known_bits_ = std::exchange(other.repeated_known_bits_, std::uint32_t{0});
        }
        return *this;
    }
    ~http_request_header_block() {
        release();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] const http_header_view& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_[index];
    }
    const http_header_view& operator[](std::size_t) const&& = delete;
    [[nodiscard]] http_field_kind kind_at(std::size_t index) const noexcept {
        return kinds_[index];
    }
    [[nodiscard]] std::span<const http_header_view> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return {fields_, size_};
    }
    std::span<const http_header_view> fields() const&& = delete;
    void mark_repeated_known_slot(std::size_t slot) noexcept {
        if (slot < 32U) {
            repeated_known_bits_ |= (std::uint32_t{1} << slot);
        }
    }
    [[nodiscard]] bool has_repeated_known_slot(std::size_t slot) const noexcept {
        return slot < 32U && (repeated_known_bits_ & (std::uint32_t{1} << slot)) != 0;
    }

    void reserve(std::size_t count) {
        if (count > (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block exceeds field limit");
        }
        if (count <= capacity_) {
            return;
        }
        auto* memory = static_cast<std::byte*>(
            ::operator new(bytes_for(count), std::align_val_t{alignof(http_header_view)}));
        auto* replacement = reinterpret_cast<http_header_view*>(memory);
        auto* replacement_kinds = reinterpret_cast<http_field_kind*>(replacement + count);
        for (std::size_t i = 0; i < size_; ++i) {
            std::construct_at(replacement + i, fields_[i]);
            replacement_kinds[i] = kinds_[i];
        }
        const auto size = size_;
        const auto repeated_known_bits = repeated_known_bits_;
        release();
        fields_ = replacement;
        kinds_ = replacement_kinds;
        size_ = size;
        capacity_ = static_cast<std::uint16_t>(count);
        repeated_known_bits_ = repeated_known_bits;
    }

    void append(http_header_view field, http_field_kind kind) {
        if (size_ == (std::numeric_limits<std::uint16_t>::max)()) {
            throw std::logic_error("request header block is full");
        }
        if (size_ == capacity_) {
            reserve((std::min)(static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()),
                (std::max)(std::size_t{1}, std::size_t(capacity_) * 2)));
        }
        std::construct_at(fields_ + size_, field);
        kinds_[size_] = kind;
        ++size_;
    }

private:
    [[nodiscard]] static std::size_t bytes_for(std::size_t count) noexcept {
        return count * sizeof(http_header_view) + count * sizeof(http_field_kind);
    }

    void release() noexcept {
        if (fields_ != nullptr) {
            std::destroy_n(fields_, size_);
            ::operator delete(fields_, bytes_for(capacity_), std::align_val_t{alignof(http_header_view)});
        }
        fields_ = nullptr;
        kinds_ = nullptr;
        size_ = 0;
        capacity_ = 0;
        repeated_known_bits_ = 0;
    }

    http_header_view* fields_{nullptr};
    http_field_kind* kinds_{nullptr};
    std::uint16_t size_{0};
    std::uint16_t capacity_{0};
    std::uint32_t repeated_known_bits_{0};
};

}  // namespace detail

class http_request final {
public:
    // Owns the compact descriptor block; field text still borrows protocol input.
    // Moving transfers it.
    http_request(const http_request&) = delete;
    http_request& operator=(const http_request&) = delete;
    http_request(http_request&&) noexcept = default;
    http_request& operator=(http_request&& other) noexcept;

    [[nodiscard]] std::string_view method() const noexcept RUVIA_LIFETIMEBOUND {
        return method_;
    }

    [[nodiscard]] http_known_method known_method() const noexcept {
        return known_method_;
    }

    // Returns the request to its default-constructed state: releases owned header
    // descriptors and clears all borrowed message views.
    void reset() noexcept {
        *this = http_request();
    }

    [[nodiscard]] std::string_view target() const noexcept RUVIA_LIFETIMEBOUND {
        return target_;
    }

    [[nodiscard]] std::string_view scheme() const noexcept RUVIA_LIFETIMEBOUND {
        return scheme_;
    }

    [[nodiscard]] std::string_view authority() const noexcept RUVIA_LIFETIMEBOUND {
        return authority_;
    }

    // The HTTP/2 :protocol pseudo-field (RFC 8441 extended CONNECT). Empty for
    // HTTP/1 requests and for HTTP/2 requests that carry no :protocol.
    [[nodiscard]] std::string_view protocol() const noexcept RUVIA_LIFETIMEBOUND {
        return protocol_;
    }

    [[nodiscard]] http_request_target_form target_form() const noexcept {
        return target_form_;
    }

    [[nodiscard]] std::string_view path() const noexcept RUVIA_LIFETIMEBOUND {
        return path_;
    }

    [[nodiscard]] std::string_view query_string() const noexcept RUVIA_LIFETIMEBOUND {
        return query_string_;
    }

    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

    // Semantic regular fields, not a byte-for-byte copy of the wire header
    // block. HTTP/1 preserves accepted field-name spelling but removes field
    // framing/OWS and replaces a conflicting Host value for absolute- or
    // authority-form targets. HTTP/2 names are lowercase, pseudo-fields are
    // exposed through method()/scheme()/authority()/protocol()/target(), split Cookie
    // fields are coalesced, and Host is synthesized from a valid :authority
    // when absent. Repeated regular fields retain wire order.
    // Content-Type is retained as metadata without parsing media-type syntax;
    // interpreting or rejecting that media type belongs to the application.
    // Content-Encoding likewise remains metadata at admission; content-coding
    // parsers validate the complete stack before a decoder is selected.
    // Origin and CORS preflight fields are metadata, not transport framing.
    // Applications validate them before deriving any cross-origin permission.
    [[nodiscard]] std::span<const http_header_view> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_.fields();
    }
    [[nodiscard]] std::span<const http_header_view> headers() const&& = delete;

    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(reinterpret_cast<const char*>(body_.data()), body_.size());
    }
    std::string_view body() const&& = delete;

    [[nodiscard]] std::span<const std::byte> body_bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    [[nodiscard]] std::span<const std::byte> body_bytes() const&& = delete;

    // Case-insensitive semantic lookup; the last repeated field wins.
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;
    // Exact raw lookup over the encoded query string. Both `raw_name` and the
    // returned value retain their percent-encoded bytes; no application/x-www-
    // form-urlencoded `+` conversion is applied. Later duplicate fields win.
    [[nodiscard]] std::optional<std::string_view> last_raw_query_value(
        std::string_view raw_name) const noexcept RUVIA_LIFETIMEBOUND;
    // Case-sensitive Cookie lookup; the last matching pair across field lines wins.
    // The returned view borrows the corresponding header text.
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept RUVIA_LIFETIMEBOUND;

private:
    friend struct detail::http_request_access;

    static constexpr std::size_t cached_header_slots = 32;

    http_request() noexcept = default;

    std::string_view method_;
    http_known_method known_method_{http_known_method::unknown};
    std::string_view target_;
    std::string_view scheme_;
    std::string_view authority_;
    std::string_view protocol_;
    std::string_view path_;
    std::string_view query_string_;
    http_protocol_version protocol_version_{http_protocol_version::http11};
    http_request_target_form target_form_{http_request_target_form::origin};
    detail::http_request_header_block headers_{};
    // One-based indices; zero means absent. No duplicated string views.
    std::array<std::uint16_t, cached_header_slots> cached_headers_{};
    std::span<const std::byte> body_{};
};

// Header descriptors live on the heap rather than the request's coroutine frame.
// The protocol field-count limit is independent of this object's layout.

// Constructs a semantic request directly, without parsing a wire-format head.
// Header names/values and body bytes are borrowed from the caller; the exact
// header span count is used to allocate descriptors. The caller must keep all
// supplied text/body storage alive while the returned request is in use.
// Invalid method, target, or header fields throw http_protocol_error. Header
// fields pass the same recipient field rules as the HTTP/1 and HTTP/2 parsers,
// so a repeated singleton field such as Range or If-Range throws instead of
// resolving to one of its lines. A header section outside the
// http_field_section_budget rule (max_http_header_bytes, max_http_header_fields)
// throws the 431 header_too_large error. Body framing fields are intentionally
// not interpreted because this API represents an already-parsed request.
[[nodiscard]] http_request make_parsed_http_request(
    borrowed_text method, borrowed_text target, std::span<const http_header_view> headers,
    borrowed_span<std::byte> body = {});

// HAProxy PROXY protocol, versions 1 and 2
// (https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt).

enum class proxy_protocol_version : std::uint8_t {
    v1,
    v2,
};

// Spec §2.2 command nibble; a v1 header is always proxy.
enum class proxy_protocol_command : std::uint8_t {
    local,
    proxy,
};

// Spec §2.2 transport protocol nibble. v1 TCP4/TCP6 are stream and UNKNOWN is
// unspecified.
enum class proxy_protocol_transport : std::uint8_t {
    unspecified,
    stream,
    datagram,
};

// One v2 Type-Length-Value vector (spec §2.2). The value borrows the parser.
struct proxy_protocol_tlv final {
    std::uint8_t type_{0};
    std::span<const std::byte> value_{};
};

struct proxy_protocol_parser_options final {
    // The whole v2 header: the 16-byte fixed part, the address block and the
    // TLVs. It must leave room for an AF_INET6 address block (16 + 36 bytes).
    // A v1 line is always capped at its spec §2.1 maximum of 107 bytes
    // including CRLF.
    std::size_t max_v2_header_bytes_{4096};
};

enum class proxy_protocol_parser_state : std::uint8_t {
    active,
    // A complete header was parsed; header() is available.
    finished,
    // The input was not a valid PROXY protocol header, exceeded a limit, or
    // the parser could not buffer it.
    failed,
};

// A parsed header. Its TLV views borrow the parser that produced it.
class proxy_protocol_header final {
public:
    [[nodiscard]] constexpr proxy_protocol_version version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr proxy_protocol_command command() const noexcept {
        return command_;
    }
    [[nodiscard]] constexpr proxy_protocol_transport transport() const noexcept {
        return transport_;
    }
    // A PROXY command with AF_INET/AF_INET6 and a stream or datagram transport
    // (v1 TCP4/TCP6): the original client and the destination it reached.
    // std::nullopt for LOCAL, v1 UNKNOWN, AF_UNSPEC, AF_UNIX and an UNSPEC
    // transport: the receiver keeps the real connection endpoints
    // (spec §2.1, §2.2).
    [[nodiscard]] constexpr std::optional<ip_endpoint> source() const noexcept {
        return source_;
    }
    [[nodiscard]] constexpr std::optional<ip_endpoint> destination() const noexcept {
        return destination_;
    }
    // Every TLV after the address block of a v2 PROXY header with a known
    // address family, raw and in wire order. Empty for v1, for LOCAL (whose
    // block is opaque) and for AF_UNSPEC (whose address length is unknown).
    [[nodiscard]] constexpr std::span<const proxy_protocol_tlv> tlvs() const& noexcept RUVIA_LIFETIMEBOUND {
        return tlvs_;
    }
    std::span<const proxy_protocol_tlv> tlvs() const&& = delete;

private:
    friend class proxy_protocol_parser;

    constexpr proxy_protocol_header() noexcept = default;

    proxy_protocol_version version_{proxy_protocol_version::v1};
    proxy_protocol_command command_{proxy_protocol_command::proxy};
    proxy_protocol_transport transport_{proxy_protocol_transport::unspecified};
    std::optional<ip_endpoint> source_{};
    std::optional<ip_endpoint> destination_{};
    std::span<const proxy_protocol_tlv> tlvs_{};
};

struct proxy_protocol_parse_result final {
    // The header ended within this input.
    bool complete_{false};
    // Header bytes only: never a byte after the header's end, so the caller
    // hands input[consumed_bytes_..] to the next protocol. Without
    // complete_, every input byte was consumed and more are needed.
    std::size_t consumed_bytes_{0};
};

class proxy_protocol_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Sans-I/O PROXY protocol header parser for the start of one connection, fed
// in chunks split at any byte. The first bytes must begin the v1 "PROXY "
// signature or the 12-byte v2 signature; there is no passthrough of other
// input. Each byte is scanned once and only header bytes are buffered.
// Invalid or over-limit input fails closed: parse() throws
// proxy_protocol_error and the parser becomes failed. Only reset() leaves the
// finished or failed state; it keeps buffer capacity, bounded by the v2
// header limit.
class proxy_protocol_parser final {
public:
    // Throws std::invalid_argument when max_v2_header_bytes_ is below 52.
    explicit proxy_protocol_parser(proxy_protocol_parser_options options = {});
    proxy_protocol_parser(const proxy_protocol_parser&) = delete;
    proxy_protocol_parser& operator=(const proxy_protocol_parser&) = delete;
    // The moved-to parser takes over the buffers that header() views borrow.
    proxy_protocol_parser(proxy_protocol_parser&&) noexcept = default;
    proxy_protocol_parser& operator=(proxy_protocol_parser&&) = delete;
    ~proxy_protocol_parser() = default;

    // A parser that is not active throws std::logic_error.
    [[nodiscard]] proxy_protocol_parse_result parse(std::span<const std::byte> input) &;
    proxy_protocol_parse_result parse(std::span<const std::byte> input) && = delete;

    [[nodiscard]] proxy_protocol_parser_state state() const noexcept {
        return state_;
    }
    // Finished parsers only; otherwise throws std::logic_error. Its views stay
    // valid until reset() or destruction.
    [[nodiscard]] const proxy_protocol_header& header() const& RUVIA_LIFETIMEBOUND;
    const proxy_protocol_header& header() const&& = delete;

    void reset() noexcept;

private:
    enum class parse_phase : std::uint8_t {
        signature,
        v1_line,
        v2_fixed,
        v2_block,
    };

    [[nodiscard]] proxy_protocol_parse_result parse_bytes(std::span<const std::byte> input);
    [[nodiscard]] std::size_t consume_signature(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v1_line(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v2_fixed(std::span<const std::byte> input, std::size_t position);
    [[nodiscard]] std::size_t consume_v2_block(std::span<const std::byte> input, std::size_t position);
    void finish_v1();
    void finish_v2();

    // Every header byte received so far. A vector, not a string: moving it
    // keeps its storage, so the header's views survive a parser move.
    std::vector<std::byte> buffer_;
    std::vector<proxy_protocol_tlv> tlvs_;
    proxy_protocol_header header_{};
    std::size_t max_v2_header_bytes_;
    // The v2 header size (16 + length), known once the fixed part is complete.
    std::size_t v2_header_size_{0};
    std::uint8_t v2_family_{0};
    parse_phase phase_{parse_phase::signature};
    proxy_protocol_parser_state state_{proxy_protocol_parser_state::active};
};

enum class http_forwarded_header_kind : std::uint8_t {
    auto_detect,
    rfc7239_forwarded,
    comma_separated_list,
    single_ip,
};

struct http_forwarded_header_spec final {
    http_forwarded_header_spec() noexcept = default;

    http_forwarded_header_spec(std::string_view name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    http_forwarded_header_spec(const char* name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : http_forwarded_header_spec(std::string_view(name), kind) {}

    http_forwarded_header_spec(const std::string& name,
        http_forwarded_header_kind kind = http_forwarded_header_kind::auto_detect)
        : name_(name),
          kind_(kind) {}

    std::string name_;
    http_forwarded_header_kind kind_{http_forwarded_header_kind::auto_detect};
};

struct http_client_ip_config final {
    http_client_ip_config() noexcept = default;

    bool enabled_{true};
    // Trusted proxy IP literals or CIDR ranges (e.g. "127.0.0.1", "10.0.0.0/8", "::1").
    std::vector<std::string> trusted_proxies_;
    // Priority-ordered forwarding headers. When empty, defaults to
    // {"X-Forwarded-For", "Forwarded"}.
    std::vector<http_forwarded_header_spec> forwarding_headers_;
    // Enable only when every trusted proxy sanitizes and appends matching
    // X-Forwarded-For / X-Forwarded-Proto elements.
    bool trust_x_forwarded_proto_{false};
};

struct http_client_ip_result final {
    ip_address address_{};
    bool from_forwarded_header_{false};
    std::optional<http_scheme> scheme_{};

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr bool from_forwarded_header() const noexcept {
        return from_forwarded_header_;
    }
    [[nodiscard]] constexpr std::optional<http_scheme> scheme() const noexcept {
        return scheme_;
    }

    friend constexpr bool operator==(const http_client_ip_result&, const http_client_ip_result&) noexcept = default;
};

// Stateless client IP extractor for deployments behind trusted reverse proxies.
class http_client_ip_extractor final {
public:
    http_client_ip_extractor() noexcept
        : enabled_(false) {}

    explicit http_client_ip_extractor(const http_client_ip_config& config);

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return enabled_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !enabled_ || trusted_cidrs_.empty();
    }
    [[nodiscard]] constexpr bool trust_x_forwarded_proto() const noexcept {
        return trust_x_forwarded_proto_;
    }
    [[nodiscard]] bool is_trusted_proxy(const ip_address& address) const noexcept;
    [[nodiscard]] bool is_trusted_proxy(std::string_view address_text) const noexcept;

    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] http_client_ip_result resolve(
        const ip_address& peer_ip, const http_request& request) const noexcept;

    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept;
    [[nodiscard]] std::optional<http_client_ip_result> resolve(
        std::string_view peer_ip, const http_request& request) const noexcept;

private:
    struct compiled_header final {
        std::string name_;
        http_forwarded_header_kind kind_{http_forwarded_header_kind::comma_separated_list};
    };

    std::vector<ip_cidr> trusted_cidrs_;
    std::vector<compiled_header> headers_;
    bool enabled_{true};
    bool trust_x_forwarded_proto_{false};
};

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    std::span<const http_header_view> headers) noexcept {
    return extractor.resolve(peer_ip, headers);
}

[[nodiscard]] inline http_client_ip_result extract_http_client_ip(
    const http_client_ip_extractor& extractor, const ip_address& peer_ip,
    const http_request& request) noexcept {
    return extractor.resolve(peer_ip, request);
}

[[nodiscard]] constexpr bool is_tls_grease_value(std::uint16_t value) noexcept {
    return (value & 0x0f0fU) == 0x0a0aU && (value >> 8U) == (value & 0xffU);
}

struct tls_client_hello_fingerprints final {
    std::string ja3_;
    std::string ja3_raw_;
    std::string ja4_;
    std::string peetprint_;
    std::string peetprint_hash_;
};

// Parsed TLS ClientHello handshake fields with GREASE values filtered out.
class tls_client_hello_info final {
public:
    tls_client_hello_info() noexcept = default;
    [[nodiscard]] constexpr std::uint16_t legacy_version() const noexcept {
        return legacy_version_;
    }
    [[nodiscard]] std::uint16_t version() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> cipher_suites() const& noexcept RUVIA_LIFETIMEBOUND {
        return cipher_suites_;
    }
    std::span<const std::uint16_t> cipher_suites() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> extensions() const& noexcept RUVIA_LIFETIMEBOUND {
        return extensions_;
    }
    std::span<const std::uint16_t> extensions() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_groups() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_groups_;
    }
    std::span<const std::uint16_t> supported_groups() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> ec_point_formats() const& noexcept RUVIA_LIFETIMEBOUND {
        return ec_point_formats_;
    }
    std::span<const std::uint8_t> ec_point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::string> alpn() const& noexcept RUVIA_LIFETIMEBOUND {
        return alpn_;
    }
    std::span<const std::string> alpn() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> signature_algorithms() const& noexcept RUVIA_LIFETIMEBOUND {
        return signature_algorithms_;
    }
    std::span<const std::uint16_t> signature_algorithms() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_versions() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_versions_;
    }
    std::span<const std::uint16_t> supported_versions() const&& = delete;
    [[nodiscard]] std::string_view server_name() const& noexcept RUVIA_LIFETIMEBOUND {
        return server_name_;
    }
    std::string_view server_name() const&& = delete;

    [[nodiscard]] std::string ja3_raw() const;
    [[nodiscard]] std::string ja3() const;
    [[nodiscard]] std::string ja4(bool quic = false) const;
    [[nodiscard]] std::string peetprint() const;
    [[nodiscard]] std::string peetprint_hash() const;
    [[nodiscard]] tls_client_hello_fingerprints fingerprints(bool quic = false) const;

    std::uint16_t legacy_version_{0};
    std::vector<std::uint16_t> cipher_suites_;
    std::vector<std::uint16_t> extensions_;
    std::vector<std::uint16_t> supported_groups_;
    std::vector<std::uint8_t> ec_point_formats_;
    std::vector<std::string> alpn_;
    std::vector<std::uint16_t> signature_algorithms_;
    std::vector<std::uint16_t> supported_versions_;
    std::string server_name_;
};

[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::span<const std::byte> message);
[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(
    std::string_view message);

[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::span<const std::byte> message, bool quic = false);
[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(
    std::string_view message, bool quic = false);
[[nodiscard]] inline tls_client_hello_fingerprints compute_tls_client_hello_fingerprints(
    const tls_client_hello_info& hello, bool quic = false) {
    return hello.fingerprints(quic);
}

}  // namespace ruvia

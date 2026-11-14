#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

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
concept http_temporary_owning_char_string = is_http_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

// std::span<const element_type> parameters bind every contiguous range,
// including temporary std::string, std::vector, and std::array values. Use this
// shared predicate on deleted overloads of span entry points whose results
// borrow the input; borrowed ranges (span, string_view) and lvalues still bind.
template <typename range_type, typename element_type>
concept http_temporary_owning_range = !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && !std::ranges::borrowed_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>;

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

    constexpr borrowed_text(std::string_view value) noexcept :
        value_(value) {}

    constexpr borrowed_text(const char* value) noexcept :
        value_(detail::http_borrowed_c_string_view(value)) {}

    template <typename traits_type, typename allocator_type>
    constexpr borrowed_text(const std::basic_string<char, traits_type, allocator_type>& value) noexcept :
        value_(value) {}

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
    constexpr borrowed_text& operator=(const std::basic_string<char, traits_type, allocator_type>& value) noexcept {
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
    friend constexpr bool operator==(borrowed_text left, const std::basic_string<char, traits_type, allocator_type>& right) noexcept {
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

    constexpr borrowed_span(std::span<const element_type> values) noexcept :
        values_(values) {}

    constexpr borrowed_span(std::span<element_type> values) noexcept :
        values_(values) {}

    template <typename range_type>
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) && std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
    constexpr borrowed_span(range_type&& values) noexcept :
        values_(std::ranges::data(values), std::ranges::size(values)) {}

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
        requires(!std::same_as<std::remove_cvref_t<range_type>, borrowed_span>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<const element_type>>) && (!std::same_as<std::remove_cvref_t<range_type>, std::span<element_type>>) && std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>
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
inline void http_clear_string_retaining_small(std::string& target, std::size_t retained_bytes = http_retained_string_bytes) noexcept {
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
    constexpr port_number(integer_type value) :
        value_(is_valid_value(value) ? static_cast<std::uint16_t>(value) : throw std::invalid_argument("port number must be in 0..65535")) {}

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

    constexpr port_number(std::uint16_t value, validated_tag_type) noexcept :
        value_(value) {}

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

    [[nodiscard]] static constexpr ip_address ipv6(const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept {
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
        return family_ == ip_address_family::ipv6 && scope_id_ == 0 && bytes_[0] == 0 && bytes_[1] == 0 && bytes_[2] == 0 && bytes_[3] == 0 && bytes_[4] == 0 && bytes_[5] == 0 && bytes_[6] == 0 && bytes_[7] == 0 && bytes_[8] == 0 && bytes_[9] == 0 && bytes_[10] == 0xff && bytes_[11] == 0xff;
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
    constexpr ip_endpoint(ip_address address, std::uint16_t port) noexcept :
        address_(address),
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
[[nodiscard]] inline std::string_view http_format_unsigned(std::span<char, http_max_unsigned_decimal_size> output, std::uint64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

// Sign plus the 19 digits of the most negative std::int64_t.
inline constexpr std::size_t http_max_signed_decimal_size = 20;

[[nodiscard]] inline std::string_view http_format_signed(std::span<char, http_max_signed_decimal_size> output, std::int64_t value) noexcept {
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
void http_append_formatted_number(std::string& output, number_type value, [[maybe_unused]] const char* error_message) {
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
void http_append_formatted_finite_number(std::string& output, number_type value, const char* finite_error_message, const char* format_error_message) {
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

[[nodiscard]] inline std::size_t http_forward_encoded_groups(const void* output, const void* input, std::size_t groups) noexcept {
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
    return http_encode_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
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
    const auto source = aliased ? std::span(reinterpret_cast<const unsigned char*>(output.data()) + offset, bytes.size()) : bytes;
    http_encode_lower_hex(output.data() + start, source);
}

inline void http_append_lower_hex(std::string& output, std::string_view bytes) {
    http_append_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// RFC 4648 section 4 (standard) and section 5 (URL-safe) alphabets.
inline constexpr std::string_view http_base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view http_base64_url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum class http_base64_charset : std::uint8_t {
    standard,  // RFC 4648 section 4: '+' and '/'
    url_safe,  // RFC 4648 section 5: '-' and '_'
};

enum class http_base64_padding : std::uint8_t {
    required,   // '=' completes the final quantum; length is a multiple of 4
    forbidden,  // unpadded (RFC 4648 section 3.2), e.g. JWS base64url
};

// Throws std::length_error when the output length cannot fit std::size_t.
[[nodiscard]] constexpr std::size_t http_base64_encoded_size(std::size_t input_size, http_base64_padding padding = http_base64_padding::required) {
    const auto groups = input_size / 3;
    const auto remainder = input_size % 3;
    const std::size_t tail = remainder == 0 ? 0 : (padding == http_base64_padding::required ? 4 : remainder + 1);
    if (groups > ((std::numeric_limits<std::size_t>::max)() - tail) / 4) {
        throw std::length_error("base64 output length is not representable");
    }
    return groups * 4 + tail;
}

// The single base64 encoder. `output` must have room for
// http_base64_encoded_size(input.size(), padding) characters and may overlap
// `input` anywhere, including in place.
inline void http_base64_encode(char* output, std::span<const std::uint8_t> input, http_base64_charset charset = http_base64_charset::standard, http_base64_padding padding = http_base64_padding::required) noexcept {
    const auto alphabet = charset == http_base64_charset::url_safe ? http_base64_url_alphabet : http_base64_alphabet;
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
[[nodiscard]] constexpr std::optional<std::size_t> http_base64_decode(std::string_view input, std::span<std::uint8_t> output, http_base64_charset charset, http_base64_padding padding) noexcept {
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
        return charset == http_base64_charset::url_safe ? http_base64_url_decode_char(c) : http_base64_decode_char(c);
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
    if ((first | second | third) < 0 || (remainder == 2 && (second & 0x0F) != 0) || (remainder == 3 && (third & 0x03) != 0)) {
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
        ? std::ranges::any_of(prefix, [](char c) noexcept {
        return c == '%' || c == '+';
    })
        : std::ranges::any_of(prefix, [](char c) noexcept {
        return c == '%';
    });
    if (encoded) {
        return true;
    }
    if (prefix.size() == value.size()) {
        return false;
    }
    value.remove_prefix(prefix.size());
    return (mode == url_decode_mode::form ? value.find_first_of("%+") : value.find('%')) != std::string_view::npos;
}

// Decodes the pct-encoded triplet starting at input[index] (RFC 3986 2.1):
// '%' followed by two hex digits. Returns no value when the triplet is
// truncated, does not start with '%', or contains a non-hex digit.
[[nodiscard]] constexpr std::optional<unsigned char> http_try_decode_percent_byte(std::string_view input, std::size_t index) noexcept {
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
[[nodiscard]] bool dispatch_url_encoded_pair_visitor(visitor_type& visitor, std::string_view name, std::string_view value) {
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
[[nodiscard]] std::optional<std::string_view> decode_url_component(borrowed_text text, std::string& storage, url_decode_mode mode = url_decode_mode::percent);

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
            const auto value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
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

}  // namespace ruvia

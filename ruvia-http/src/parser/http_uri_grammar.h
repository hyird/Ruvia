#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia::detail {

// Borrowed field descriptors stay inline until their reserved or appended count
// needs heap overflow. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    http_header_descriptor_storage() noexcept = default;

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&& other) noexcept
        : overflow_(std::exchange(other.overflow_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, inline_capacity)) {
        if (overflow_ == nullptr) {
            std::copy_n(other.inline_.data(), size_, inline_.data());
        }
    }
    http_header_descriptor_storage& operator=(http_header_descriptor_storage&&) = delete;

    ~http_header_descriptor_storage() {
        if (overflow_ != nullptr) {
            std::allocator<field_type>{}.deallocate(overflow_, capacity_);
        }
    }

    void reserve(std::size_t requested_capacity) {
        if (requested_capacity <= capacity_) {
            return;
        }
        reallocate(requested_capacity);
    }

    void append(field_type field) {
        if (size_ == capacity_) {
            grow();
        }
        auto* destination = overflow_ != nullptr ? overflow_ : inline_.data();
        destination[size_++] = field;
    }

    void clear() noexcept {
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::span<const field_type> view() const& noexcept {
        return {overflow_ != nullptr ? overflow_ : inline_.data(), size_};
    }
    std::span<const field_type> view() const&& = delete;

private:
    static constexpr std::size_t inline_capacity = 32;

    void grow() {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (capacity_ >= maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        const auto next_capacity = capacity_ <= maximum / 2 ? capacity_ * 2 : maximum;
        reallocate(next_capacity);
    }

    void reallocate(std::size_t next_capacity) {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (next_capacity > maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        std::allocator<field_type> allocator;
        auto* next = allocator.allocate(next_capacity);
        // Trivial descriptors cannot throw while copying. Publish only after
        // allocation succeeds, preserving the current fields on failure.
        const auto fields = view();
        std::uninitialized_copy_n(fields.data(), fields.size(), next);
        if (overflow_ != nullptr) {
            allocator.deallocate(overflow_, capacity_);
        }
        overflow_ = next;
        capacity_ = next_capacity;
    }

    // POD request slots stay uninitialized until appended. Every readable slot
    // is fully assigned before publication, without zeroing unused slots.
    std::array<field_type, inline_capacity> inline_;
    field_type* overflow_{nullptr};
    std::size_t size_{0};
    std::size_t capacity_{inline_capacity};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The one Extended CONNECT WebSocket bootstrap field list for HTTP/2 (RFC
// 8441) and HTTP/3 (RFC 9220): the negotiation's Sec-WebSocket-* request
// fields, then the application headers. The descriptors borrow both.
[[nodiscard]] inline http_header_descriptor_storage<http_header_view> stage_websocket_extended_connect_fields(
    const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers) {
    const auto negotiation_fields = negotiation.request_fields();
    http_header_descriptor_storage<http_header_view> staged;
    staged.reserve(negotiation_fields.size() + headers.size());
    for (const auto& field : negotiation_fields) {
        staged.append(http_header_view{field.name(), field.value()});
    }
    for (const auto& field : headers) {
        staged.append(field);
    }
    return staged;
}

}  // namespace ruvia::detail

// RFC 3986 syntax, one level below any HTTP-specific rule: which byte sequences
// form a legal URI component, userinfo, port, IPv4 / IPv6 / IPvFuture literal or
// reg-name. Nothing here knows about request targets, Host fields or origins --
// those rules live in http_request_target.h and http_serialized_origin.cpp.

namespace ruvia::detail {

[[nodiscard]] inline bool is_decimal_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] inline constexpr bool is_uri_lower_alpha(unsigned char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] inline constexpr bool is_uri_alpha(unsigned char byte) noexcept {
    return is_uri_lower_alpha(byte) || (byte >= 'A' && byte <= 'Z');
}

// RFC 3986 section 2.3 unreserved. Percent-encoding one of these octets is
// equivalent to spelling it literally, which host comparison relies on.
[[nodiscard]] inline constexpr bool is_unreserved_byte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || is_uri_alpha(byte) ||
           byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Literal pchar bytes. The caller validates percent-encoded triplets separately.
[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_uri_port(std::string_view value) noexcept;
[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept;

// Split RFC 3986 host [ ":" port ] (host may be an IP-literal in brackets)
// without validating either part. False when brackets are unbalanced, text
// follows "]" without ":", or the port contains another ":".
[[nodiscard]] bool split_uri_host_port(
    std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept;

// Parse a decimal port, rejecting anything that does not fit 16 bits.
[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept;

// Validate a percent-encoded component: unreserved / sub-delims / pct-encoded,
// plus ':' and '@', with '/' and '?' admitted only where the component allows them.
[[nodiscard]] bool is_valid_uri_component(
    std::string_view value, bool allow_slash, bool allow_question) noexcept;

// Address octets/groups in wire order; no allocation or platform IP parser.
// IPv6 and IPvFuture literals are supplied without surrounding brackets.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> parse_ipv4_address(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::array<std::uint16_t, 8>> parse_ipv6_address(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_target_view {
    std::string_view scheme_;
    std::string_view path_;
    std::string_view query_;
    std::string_view authority_;
    http_request_target_form form_{http_request_target_form::origin};
};

// Validates the byte repertoire shared by the four HTTP request-target forms.
// Component-specific rules (for example, '[' and ']' being legal only in an
// IP-literal authority) are applied by parse_request_target /
// is_valid_origin_form_target.
[[nodiscard]] bool is_valid_request_target_bytes(std::string_view target) noexcept;

// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
[[nodiscard]] bool is_valid_uri_scheme(std::string_view value) noexcept;

[[nodiscard]] constexpr std::uint16_t http_scheme_default_port(http_scheme scheme) noexcept {
    return scheme == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80};
}

[[nodiscard]] std::uint16_t http_uri_scheme_default_port(std::string_view scheme) noexcept;
[[nodiscard]] bool is_valid_uri_authority(std::string_view value) noexcept;

// Strict origin-form starts with '/'. Asterisk-form is a separate target form
// and is exposed explicitly so callers cannot lose its OPTIONS-only contract.
[[nodiscard]] bool is_valid_origin_form_target(std::string_view target) noexcept;
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(
    http_known_method method, std::string_view target) noexcept;

// Host field syntax. An empty field is valid when the target URI has no
// authority (RFC 9112 section 3.2); callers that require a usable HTTP origin
// must additionally require a non-empty value.
[[nodiscard]] bool is_valid_host_header(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool is_valid_http_host(std::string_view value) noexcept;
[[nodiscard]] bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool parse_request_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept;
template <http_temporary_owning_char_string target_type>
bool parse_request_target(http_known_method, target_type&&, request_target_view&) = delete;
[[nodiscard]] bool authority_matches_host(
    std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept;

}  // namespace ruvia::detail

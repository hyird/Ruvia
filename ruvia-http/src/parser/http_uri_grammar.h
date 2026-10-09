#pragma once

#include <cstdint>
#include <string_view>
#include <system_error>
#include <variant>

// RFC 3986 syntax, one level below any HTTP-specific rule: which byte sequences
// form a legal URI component, userinfo, port, IPv4 / IPv6 / IPvFuture literal or
// reg-name. Nothing here knows about request targets, Host fields or origins --
// those rules live in http_request_target.h and http_serialized_origin.h.

namespace ruvia::detail {

[[nodiscard]] inline bool is_decimal_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// RFC 3986 section 2.3 unreserved. Percent-encoding one of these octets is
// equivalent to spelling it literally, which host comparison relies on.
[[nodiscard]] inline constexpr bool is_unreserved_byte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Literal pchar bytes. The caller validates percent-encoded triplets separately.
[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept;

// Parse a decimal port, rejecting anything that does not fit 16 bits.
[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept;

// Validate a percent-encoded component: unreserved / sub-delims / pct-encoded,
// plus ':' and '@', with '/' and '?' admitted only where the component allows them.
[[nodiscard]] bool is_valid_uri_component(
    std::string_view value, bool allow_slash, bool allow_question) noexcept;
[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_uri_port(std::string_view value) noexcept;

[[nodiscard]] bool parse_ipv4_address(std::string_view value) noexcept;
// Both take the literal without its surrounding brackets.
[[nodiscard]] bool is_valid_ipv6_literal(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept;

}  // namespace ruvia::detail

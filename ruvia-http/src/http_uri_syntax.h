#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <variant>

#include "borrowed_text.h"
#include "http_message.h"
#include "http_request.h"

// RFC 3986 syntax and the HTTP rules built directly on it: URI components,
// IPv4 / IPv6 / IPvFuture literals, reg-names, schemes, authorities, the four
// request-target forms, Host field syntax and authority/Host agreement.
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
    return (byte >= '0' && byte <= '9') || is_uri_alpha(byte) || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Literal pchar bytes. The caller validates percent-encoded triplets separately.
[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_uri_port(std::string_view value) noexcept;
[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept;

// Split RFC 3986 host [ ":" port ] (host may be an IP-literal in brackets)
// without validating either part. False when brackets are unbalanced, text
// follows "]" without ":", or the port contains another ":".
[[nodiscard]] bool split_uri_host_port(std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept;

// Parse a decimal port, rejecting anything that does not fit 16 bits.
[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept;

// Validate a percent-encoded component: unreserved / sub-delims / pct-encoded,
// plus ':' and '@', with '/' and '?' admitted only where the component allows them.
[[nodiscard]] bool is_valid_uri_component(std::string_view value, bool allow_slash, bool allow_question) noexcept;

// Address octets/groups in wire order; no allocation or platform IP parser.
// IPv6 and IPvFuture literals are supplied without surrounding brackets.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> parse_ipv4_address(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::array<std::uint16_t, 8>> parse_ipv6_address(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept;

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
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(http_known_method method, std::string_view target) noexcept;

// Host field syntax. An empty field is valid when the target URI has no
// authority (RFC 9112 section 3.2); callers that require a usable HTTP origin
// must additionally require a non-empty value.
[[nodiscard]] bool is_valid_host_header(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool is_valid_http_host(std::string_view value) noexcept;
[[nodiscard]] bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool parse_request_target(http_known_method method, std::string_view target, request_target_view& output) noexcept;
template <http_temporary_owning_char_string target_type>
bool parse_request_target(http_known_method, target_type&&, request_target_view&) = delete;
[[nodiscard]] bool authority_matches_host(std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept;

}  // namespace ruvia::detail

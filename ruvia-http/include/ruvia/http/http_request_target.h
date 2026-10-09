#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

// Borrowed RFC 3986 HTTP authority, with brackets retained around IP literals.
struct http_authority_view final {
    std::string_view host_{};
    std::optional<std::uint16_t> port_{};
};
[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port. Invalid authorities never compare equal.
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, std::uint16_t default_port) noexcept;

}  // namespace ruvia

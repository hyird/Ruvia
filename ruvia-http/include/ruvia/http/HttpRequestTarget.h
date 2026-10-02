#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/WebSocket clients.
[[nodiscard]] bool isValidHttpOriginFormTarget(std::string_view target) noexcept;

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool isValidHttpConnectAuthority(std::string_view authority) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool isValidHttpIpv4Literal(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpIpv6Literal(std::string_view value) noexcept;

// Borrowed RFC 3986 HTTP authority, with brackets retained around IP literals.
struct HttpAuthorityView final {
    std::string_view host{};
    std::optional<std::uint16_t> port{};
};
[[nodiscard]] std::optional<HttpAuthorityView> parseHttpAuthority(BorrowedText value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parseHttpAuthorityHost(BorrowedText value) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port. Invalid authorities never compare equal.
[[nodiscard]] bool httpAuthoritiesEqual(BorrowedText left, BorrowedText right, std::uint16_t defaultPort) noexcept;

}  // namespace ruvia

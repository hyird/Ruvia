#pragma once

#include <optional>
#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/WebSocket clients.
[[nodiscard]] bool isValidHttpOriginFormTarget(std::string_view target) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool isValidHttpIpv4Literal(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpIpv6Literal(std::string_view value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parseHttpAuthorityHost(BorrowedText value) noexcept;

}  // namespace ruvia

#pragma once

#include <ctime>
#include <string_view>

namespace ruvia::detail {

// Full "Date: <value>\r\n" line for HTTP/1 text response heads. Empty when the
// clock reports failure (-1) or a date outside the four-digit wire year range.
[[nodiscard]] std::string_view cachedDateHeader(std::time_t now = std::time(nullptr)) noexcept;

// Bare date value (no field name, no CRLF) for HPACK-encoded HTTP/2 headers.
[[nodiscard]] std::string_view cachedDateValue(std::time_t now = std::time(nullptr)) noexcept;

}  // namespace ruvia::detail

#pragma once

#include <string_view>

namespace ruvia::detail {

// Full "Date: <value>\r\n" line for the current second, for HTTP/1 text response
// heads. Empty when the clock reports failure (-1) or a date outside the
// four-digit wire year range.
[[nodiscard]] std::string_view cached_date_header() noexcept;

// Bare date value (no field name, no CRLF) for HPACK-encoded HTTP/2 headers.
[[nodiscard]] std::string_view cached_date_value() noexcept;

}  // namespace ruvia::detail

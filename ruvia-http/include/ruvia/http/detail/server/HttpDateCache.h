#pragma once

#include <string_view>

namespace ruvia::detail {

// Full "Date: <value>\r\n" line for HTTP/1 text response heads.
[[nodiscard]] std::string_view cachedDateHeader() noexcept;

// Bare date value (no field name, no CRLF) for HPACK-encoded HTTP/2 headers.
[[nodiscard]] std::string_view cachedDateValue() noexcept;

}  // namespace ruvia::detail

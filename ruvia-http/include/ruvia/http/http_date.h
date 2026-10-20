#pragma once

#include <array>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

// Wire width of an RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t http_imf_fixdate_size = 29;

// RFC 9110 IMF-fixdate, or no value if the timestamp cannot be represented.
// The fixed-size result contains the 29 wire bytes without a terminator.
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;

}  // namespace ruvia

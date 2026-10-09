#pragma once

#include <array>
#include <cstddef>
#include <ctime>
#include <optional>

namespace ruvia {

// Wire width of an RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t http_imf_fixdate_size = 29;

// RFC 9110 IMF-fixdate, or no value if the timestamp cannot be represented.
// The fixed-size result contains the 29 wire bytes without a terminator.
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;

}  // namespace ruvia

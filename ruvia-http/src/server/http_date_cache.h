#pragma once

#include <array>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia::detail {

// IMF-fixdate month names (RFC 9110 section 5.6.7), in calendar order.
inline constexpr std::array<std::string_view, 12> http_month_names{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Seconds since the epoch for UTC civil fields, or no value for an invalid
// calendar date, out-of-range clock fields or a time_t overflow. A second of 60
// is accepted as the leap second and counts into the following minute.
[[nodiscard]] std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept;

// Full "Date: <value>\r\n" line for the current second, for HTTP/1 text response
// heads. Empty when the clock reports failure (-1) or a date outside the
// four-digit wire year range.
[[nodiscard]] std::string_view cached_date_header() noexcept;

// Bare date value (no field name, no CRLF) for HPACK-encoded HTTP/2 headers.
[[nodiscard]] std::string_view cached_date_value() noexcept;
}  // namespace ruvia::detail

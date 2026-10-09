#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <string_view>
#include <variant>

#include "field/http_imf_fixdate.h"

namespace ruvia::detail {

[[nodiscard]] inline int http_month_index(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t i = 0; i < months.size(); ++i) {
        if (value == months[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] inline bool http_is_short_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] inline bool http_is_long_weekday(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] inline std::optional<int> http_parse_fixed_digits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed_value = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (c - '0');
    }
    return parsed_value;
}

[[nodiscard]] inline std::int64_t http_days_from_civil(
    int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] inline std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept {
    if (month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return std::nullopt;
    }
    constexpr std::array<int, 12> days_per_month{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const auto max_day =
        days_per_month[static_cast<std::size_t>(month - 1)] + (month == 2 && leap_year ? 1 : 0);
    if (day > max_day) {
        return std::nullopt;
    }
    const auto days =
        http_days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const auto total = days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                       static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    if constexpr (std::numeric_limits<std::time_t>::is_signed) {
        if (total < static_cast<std::int64_t>((std::numeric_limits<std::time_t>::min)()) ||
            total > static_cast<std::int64_t>((std::numeric_limits<std::time_t>::max)())) {
            return std::nullopt;
        }
    } else if (total < 0 ||
               static_cast<std::uint64_t>(total) >
                   static_cast<std::uint64_t>((std::numeric_limits<std::time_t>::max)())) {
        return std::nullopt;
    }
    return static_cast<std::time_t>(total);
}

[[nodiscard]] inline std::optional<std::time_t> http_parse_imf_fixdate(
    std::string_view value) noexcept {
    if (value.size() != 29 || !http_is_short_weekday(value.substr(0, 3)) || value[3] != ',' ||
        value[4] != ' ' || value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' || value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = http_parse_fixed_digits(value.substr(5, 2));
    const auto month = http_month_index(value.substr(8, 3));
    const auto year = http_parse_fixed_digits(value.substr(12, 4));
    const auto hour = http_parse_fixed_digits(value.substr(17, 2));
    const auto minute = http_parse_fixed_digits(value.substr(20, 2));
    const auto second = http_parse_fixed_digits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline std::optional<std::time_t> http_parse_asctime_date(
    std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !http_is_short_weekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = http_month_index(value.substr(4, 3));
    auto day_field = value.substr(8, 2);
    if (day_field.front() == ' ') {
        day_field.remove_prefix(1);
    }
    const auto day = http_parse_fixed_digits(day_field);
    const auto hour = http_parse_fixed_digits(value.substr(11, 2));
    const auto minute = http_parse_fixed_digits(value.substr(14, 2));
    const auto second = http_parse_fixed_digits(value.substr(17, 2));
    const auto year = http_parse_fixed_digits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return http_civil_to_time_t(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline int http_resolve_rfc850_year(int short_year, int current_year) noexcept {
    const auto current_century = current_year - current_year % 100;
    auto year = current_century + short_year;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > current_year + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] inline std::optional<std::time_t> http_parse_rfc850_date(
    std::string_view value, std::optional<std::time_t> reference_time = std::nullopt) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos || !http_is_long_weekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = http_parse_fixed_digits(body.substr(0, 2));
    const auto month = http_month_index(body.substr(3, 3));
    const auto short_year = http_parse_fixed_digits(body.substr(7, 2));
    const auto hour = http_parse_fixed_digits(body.substr(10, 2));
    const auto minute = http_parse_fixed_digits(body.substr(13, 2));
    const auto second = http_parse_fixed_digits(body.substr(16, 2));
    if (!day || month == 0 || !short_year || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto now = reference_time ? *reference_time : std::time(nullptr);
    if (!reference_time && now == std::time_t{-1}) {
        return std::nullopt;
    }
    const auto current_utc = http_utc_tm(now);
    if ((current_utc.index() != 0)) {
        return std::nullopt;
    }
    const int current_year = std::get<0>(current_utc).tm_year + 1900;
    auto year = http_resolve_rfc850_year(*short_year, current_year);
    // RFC 9110 section 5.6.7 applies the rolling 50-year pivot to the full
    // timestamp. Equal years alone do not establish that a date is in range.
    const std::array date_fields{month, *day, *hour, *minute, *second};
    const std::array reference_fields{std::get<0>(current_utc).tm_mon + 1, std::get<0>(current_utc).tm_mday,
        std::get<0>(current_utc).tm_hour, std::get<0>(current_utc).tm_min, std::get<0>(current_utc).tm_sec};
    if (year == current_year + 50 && date_fields > reference_fields) {
        year -= 100;
    }
    return http_civil_to_time_t(year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline std::optional<std::time_t> http_parse_http_date(std::string_view value) noexcept {
    if (const auto imf = http_parse_imf_fixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = http_parse_rfc850_date(value)) {
        return rfc850;
    }
    return http_parse_asctime_date(value);
}

}  // namespace ruvia::detail

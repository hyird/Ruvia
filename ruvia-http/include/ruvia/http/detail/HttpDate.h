#pragma once

#include "ruvia/http/detail/HttpImfFixdate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::string httpFormatDate(
    std::pmr::memory_resource* resource,
    std::time_t time) {
    const auto utc = httpUtcTm(time);
    char buffer[kImfFixdateSize];
    const auto written = httpWriteImfFixdate(buffer, utc);
    std::pmr::string output(resource);
    output.assign(buffer, written);
    return output;
}

[[nodiscard]] inline int httpMonthIndex(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (std::size_t i = 0; i < months.size(); ++i) {
        if (value == months[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

[[nodiscard]] inline bool httpIsShortWeekday(
    std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] inline bool httpIsLongWeekday(
    std::string_view value) noexcept {
    constexpr std::array<std::string_view, 7> weekdays{
        "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday", "Sunday"};
    return std::ranges::find(weekdays, value) != weekdays.end();
}

[[nodiscard]] inline std::optional<int> httpParseFixedDigits(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    int parsed = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        parsed = parsed * 10 + (c - '0');
    }
    return parsed;
}

[[nodiscard]] inline std::int64_t httpDaysFromCivil(
    int year,
    unsigned month,
    unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const auto doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

[[nodiscard]] inline std::optional<std::time_t> httpCivilToTimeT(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second) noexcept {
    if (month < 1 || month > 12 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return std::nullopt;
    }
    constexpr std::array<int, 12> daysPerMonth{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leapYear =
        year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const auto maxDay = daysPerMonth[static_cast<std::size_t>(month - 1)] +
        (month == 2 && leapYear ? 1 : 0);
    if (day > maxDay) {
        return std::nullopt;
    }
    const auto days = httpDaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const auto total = days * 86400 +
        static_cast<std::int64_t>(hour) * 3600 +
        static_cast<std::int64_t>(minute) * 60 +
        static_cast<std::int64_t>(second);
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

[[nodiscard]] inline std::optional<std::time_t> httpParseImfFixdate(
    std::string_view value) noexcept {
    if (value.size() != 29 ||
        !httpIsShortWeekday(value.substr(0, 3)) ||
        value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
        value[16] != ' ' || value[19] != ':' || value[22] != ':' || value[25] != ' ' ||
        value.substr(26, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = httpParseFixedDigits(value.substr(5, 2));
    const auto month = httpMonthIndex(value.substr(8, 3));
    const auto year = httpParseFixedDigits(value.substr(12, 4));
    const auto hour = httpParseFixedDigits(value.substr(17, 2));
    const auto minute = httpParseFixedDigits(value.substr(20, 2));
    const auto second = httpParseFixedDigits(value.substr(23, 2));
    if (!day || month == 0 || !year || !hour || !minute || !second) {
        return std::nullopt;
    }
    return httpCivilToTimeT(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline std::optional<std::time_t> httpParseAsctimeDate(
    std::string_view value) noexcept {
    if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value[19] != ' ' ||
        !httpIsShortWeekday(value.substr(0, 3))) {
        return std::nullopt;
    }
    const auto month = httpMonthIndex(value.substr(4, 3));
    auto dayField = value.substr(8, 2);
    if (dayField.front() == ' ') {
        dayField.remove_prefix(1);
    }
    const auto day = httpParseFixedDigits(dayField);
    const auto hour = httpParseFixedDigits(value.substr(11, 2));
    const auto minute = httpParseFixedDigits(value.substr(14, 2));
    const auto second = httpParseFixedDigits(value.substr(17, 2));
    const auto year = httpParseFixedDigits(value.substr(20, 4));
    if (month == 0 || !day || !hour || !minute || !second || !year) {
        return std::nullopt;
    }
    return httpCivilToTimeT(*year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline int httpResolveRfc850Year(
    int shortYear,
    int currentYear) noexcept {
    const auto currentCentury = currentYear - currentYear % 100;
    auto year = currentCentury + shortYear;
    // RFC 9110 section 5.6.7: an obsolete two-digit date that appears more
    // than 50 years in the future denotes the most recent past year with the
    // same final two digits. This is a rolling pivot, not the fixed POSIX
    // 68/69 split.
    if (year > currentYear + 50) {
        year -= 100;
    }
    return year;
}

[[nodiscard]] inline std::optional<std::time_t> httpParseRfc850Date(
    std::string_view value) noexcept {
    const auto comma = value.find(", ");
    if (comma == std::string_view::npos ||
        !httpIsLongWeekday(value.substr(0, comma))) {
        return std::nullopt;
    }
    const auto body = value.substr(comma + 2);
    if (body.size() != 22 || body[2] != '-' || body[6] != '-' || body[9] != ' ' ||
        body[12] != ':' || body[15] != ':' || body[18] != ' ' || body.substr(19, 3) != "GMT") {
        return std::nullopt;
    }
    const auto day = httpParseFixedDigits(body.substr(0, 2));
    const auto month = httpMonthIndex(body.substr(3, 3));
    const auto shortYear = httpParseFixedDigits(body.substr(7, 2));
    const auto hour = httpParseFixedDigits(body.substr(10, 2));
    const auto minute = httpParseFixedDigits(body.substr(13, 2));
    const auto second = httpParseFixedDigits(body.substr(16, 2));
    if (!day || month == 0 || !shortYear || !hour || !minute || !second) {
        return std::nullopt;
    }
    const auto currentUtc = httpUtcTm(std::time(nullptr));
    const int currentYear = currentUtc.tm_year + 1900;
    const int year = httpResolveRfc850Year(*shortYear, currentYear);
    return httpCivilToTimeT(year, month, *day, *hour, *minute, *second);
}

[[nodiscard]] inline std::optional<std::time_t> httpParseHttpDate(
    std::string_view value) noexcept {
    if (const auto imf = httpParseImfFixdate(value)) {
        return imf;
    }
    if (const auto rfc850 = httpParseRfc850Date(value)) {
        return rfc850;
    }
    return httpParseAsctimeDate(value);
}

}  // namespace ruvia::detail

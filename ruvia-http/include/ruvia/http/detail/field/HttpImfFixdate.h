#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <expected>
#include <string_view>
#include <utility>

namespace ruvia::detail {

// Number of bytes an RFC 9110 §5.6.7 IMF-fixdate occupies, e.g.
// "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t kImfFixdateSize = 29;

enum class HttpDateFormatError { kOutOfRange };

// Civil conversion is independent of the C runtime's date range and locale.
// In particular, Windows gmtime_s cannot represent pre-1970 UTC timestamps.
// Check before calendar conversion so huge time_t values cannot overflow it.
[[nodiscard]] inline std::expected<std::tm, HttpDateFormatError> httpUtcTm(std::time_t time) noexcept {
    using namespace std::chrono;
    constexpr auto first = duration_cast<seconds>(sys_days{year{0} / January / 1}.time_since_epoch()).count();
    constexpr auto end = duration_cast<seconds>(sys_days{year{10000} / January / 1}.time_since_epoch()).count();
    if (std::cmp_less(time, first) || std::cmp_greater_equal(time, end)) {
        return std::unexpected(HttpDateFormatError::kOutOfRange);
    }
    const sys_seconds instant{seconds{static_cast<seconds::rep>(time)}};
    const auto date = floor<days>(instant);
    const year_month_day calendar{date};
    const hh_mm_ss clock{instant - date};
    std::tm utc{};
    utc.tm_year = static_cast<int>(calendar.year()) - 1900;
    utc.tm_mon = static_cast<int>(static_cast<unsigned>(calendar.month())) - 1;
    utc.tm_mday = static_cast<int>(static_cast<unsigned>(calendar.day()));
    utc.tm_wday = static_cast<int>(weekday{date}.c_encoding());
    utc.tm_yday = static_cast<int>((date - sys_days{calendar.year() / January / 1}).count());
    utc.tm_hour = static_cast<int>(clock.hours().count());
    utc.tm_min = static_cast<int>(clock.minutes().count());
    utc.tm_sec = static_cast<int>(clock.seconds().count());
    return utc;
}

// Allocation-free wire value; an unrepresentable date must not be truncated or
// substituted with a different timestamp. Fixed English names are independent
// of the process locale, as required by RFC 9110 section 5.6.7.
[[nodiscard]] inline std::expected<std::array<char, kImfFixdateSize>, HttpDateFormatError>
httpFormatDate(std::time_t time) noexcept {
    const auto converted = httpUtcTm(time);
    if (!converted) {
        return std::unexpected(converted.error());
    }
    const auto& utc = *converted;
    std::array<char, kImfFixdateSize> output{};
    auto* out = output.data();
    static constexpr std::array<std::string_view, 7> dayNames{
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::array<std::string_view, 12> monthNames{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const auto wday = utc.tm_wday;
    const auto mon = utc.tm_mon;
    const long year = static_cast<long>(utc.tm_year) + 1900;

    std::size_t i = 0;
    const auto put2 = [out, &i](int value) noexcept {
        out[i++] = static_cast<char>('0' + (value / 10) % 10);
        out[i++] = static_cast<char>('0' + value % 10);
    };
    const auto put3 = [out, &i](std::string_view name) noexcept {
        out[i++] = name[0];
        out[i++] = name[1];
        out[i++] = name[2];
    };

    put3(dayNames[static_cast<std::size_t>(wday)]);
    out[i++] = ',';
    out[i++] = ' ';
    put2(utc.tm_mday);
    out[i++] = ' ';
    put3(monthNames[static_cast<std::size_t>(mon)]);
    out[i++] = ' ';
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 1000) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 100) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>((year / 10) % 10));
    out[i++] = static_cast<char>('0' + static_cast<int>(year % 10));
    out[i++] = ' ';
    put2(utc.tm_hour);
    out[i++] = ':';
    put2(utc.tm_min);
    out[i++] = ':';
    put2(utc.tm_sec);
    out[i++] = ' ';
    out[i++] = 'G';
    out[i++] = 'M';
    out[i++] = 'T';
    return output;
}

}  // namespace ruvia::detail

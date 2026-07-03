#pragma once

#include <array>
#include <cstddef>
#include <ctime>
#include <string_view>

namespace ruvia::detail {

// Number of bytes an RFC 7231 §7.1.1.1 IMF-fixdate occupies, e.g.
// "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t kImfFixdateSize = 29;

// Writes the IMF-fixdate for `utc` (a UTC std::tm, as produced by gmtime_r /
// gmtime_s) into `out`, which must have room for at least kImfFixdateSize bytes.
// Returns the number of bytes written (always kImfFixdateSize).
//
// The day-of-week and month abbreviations are emitted from fixed English tables
// rather than via strftime's locale-dependent %a/%b: RFC 7231 mandates the
// English names regardless of the process locale, so this is the single owner of
// HTTP date formatting for both the response Date header and Last-Modified.
inline std::size_t httpWriteImfFixdate(char* out, const std::tm& utc) noexcept {
    static constexpr std::array<std::string_view, 7> dayNames{
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::array<std::string_view, 12> monthNames{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const auto wday = (utc.tm_wday >= 0 && utc.tm_wday < 7) ? utc.tm_wday : 0;
    const auto mon = (utc.tm_mon >= 0 && utc.tm_mon < 12) ? utc.tm_mon : 0;
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
    return i;
}

}  // namespace ruvia::detail

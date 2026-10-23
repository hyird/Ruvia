#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The text MariaDB's text protocol sends for values its binary protocol
// carries, so a statement returns the same db_field text whichever protocol
// ran it. The server formats a double with my_gcvt() into a buffer of
// FLOATING_POINT_BUFFER - 1 characters, a float the same way with at most
// FLT_DIG significant digits, and a value of fixed scale with my_fcvt(); both
// round with dtoa, which std::to_chars reproduces: the shortest digits that
// read back as the value, or the value correctly rounded when those are too
// many.

namespace ruvia::detail {

// The decimals a field reports when its value has no fixed scale.
inline constexpr unsigned int mariadb_not_fixed_decimals = 31;
// Room for any formatted value: a double in plain notation reaches about 330
// characters.
inline constexpr std::size_t mariadb_text_capacity = 400;

using mariadb_text_buffer = std::array<char, mariadb_text_capacity>;

namespace mariadb_text_detail {

// The significant digits of a nonzero finite magnitude and where its decimal
// point falls, as dtoa returns them: 1234 at point 2 is 12.34.
struct digits final {
    std::array<char, 32> text_{};
    int length_{0};
    int point_{0};
};

// Reads std::to_chars' scientific form "d.ddde+XX".
[[nodiscard]] inline digits parse_scientific(const char* first, const char* last) noexcept {
    digits result;
    const char* cursor = first;
    for (; cursor != last && *cursor != 'e'; ++cursor) {
        if (*cursor != '.') {
            result.text_[static_cast<std::size_t>(result.length_++)] = *cursor;
        }
    }
    int exponent = 0;
    if (cursor != last) {
        (void)std::from_chars(cursor + (cursor[1] == '+' ? 2 : 1), last, exponent);
    }
    while (result.length_ > 1 && result.text_[static_cast<std::size_t>(result.length_ - 1)] == '0') {
        --result.length_;
    }
    result.point_ = exponent + 1;
    return result;
}

// dtoa's mode 4: the shortest digits that read back as `magnitude`, or, when
// those are more than `limit`, `magnitude` correctly rounded to `limit`
// significant digits.
[[nodiscard]] inline digits significant(double magnitude, int limit) noexcept {
    std::array<char, 64> scratch{};
    auto converted =
        std::to_chars(scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific);
    auto result = parse_scientific(scratch.data(), converted.ptr);
    if (result.length_ > limit) {
        converted = std::to_chars(
            scratch.data(), scratch.data() + scratch.size(), magnitude, std::chars_format::scientific, limit - 1);
        result = parse_scientific(scratch.data(), converted.ptr);
    }
    return result;
}

// Digits in plain notation: "0.00ddd", "dd.ddd" or "ddd00".
[[nodiscard]] inline char* put_plain(char* out, const digits& found) noexcept {
    if (found.point_ <= 0) {
        *out++ = '0';
        *out++ = '.';
        for (int index = found.point_; index < 0; ++index) {
            *out++ = '0';
        }
    }
    int index = 1;
    for (; index <= found.length_; ++index) {
        *out++ = found.text_[static_cast<std::size_t>(index - 1)];
        if (index == found.point_ && index < found.length_) {
            *out++ = '.';
        }
    }
    for (; index <= found.point_; ++index) {
        *out++ = '0';
    }
    return out;
}

}  // namespace mariadb_text_detail

// my_gcvt() with the server's width: plain notation while it shows every
// significant digit and the point stays within DBL_DIG places of the digits,
// else scientific ("1e20", "1.5e-20").
[[nodiscard]] inline std::size_t format_mariadb_real(mariadb_text_buffer& buffer, double value, bool single) noexcept {
    namespace text = mariadb_text_detail;
    constexpr int dbl_dig = 15;
    constexpr int flt_dig = 6;
    if (!std::isfinite(value)) {
        // dtoa reports an overflow for infinity and NaN, which my_gcvt()
        // writes as "0".
        buffer[0] = '0';
        return 1;
    }
    int width = 310;
    char* out = buffer.data();
    if (std::signbit(value)) {
        *out++ = '-';
        --width;
    }
    const double magnitude = std::fabs(value);
    if (magnitude == 0) {
        *out++ = '0';
        return static_cast<std::size_t>(out - buffer.data());
    }
    const auto found = text::significant(magnitude, single ? flt_dig : width);
    const int length = found.length_;
    const int point = found.point_;
    // The plain notation's length: "0.00ddd", "dd.ddd" or "ddd00".
    int plain_length = point;
    if (point <= 0) {
        plain_length = length - point + 2;
    } else if (point < length) {
        plain_length = length + 1;
    }
    const bool have_space = plain_length <= width;
    if (have_space && point >= -dbl_dig + 1 && (point <= dbl_dig || length > point)) {
        out = text::put_plain(out, found);
        return static_cast<std::size_t>(out - buffer.data());
    }
    *out++ = found.text_[0];
    if (length > 1) {
        *out++ = '.';
        for (int index = 1; index < length; ++index) {
            *out++ = found.text_[static_cast<std::size_t>(index)];
        }
    }
    *out++ = 'e';
    int exponent = point - 1;
    if (exponent < 0) {
        *out++ = '-';
        exponent = -exponent;
    }
    out = std::to_chars(out, buffer.data() + buffer.size(), exponent).ptr;
    return static_cast<std::size_t>(out - buffer.data());
}

// my_fcvt(): exactly `decimals` digits after the point. dtoa keeps the
// shortest digits that read back as the value when they fit, and pads them
// with zeros; otherwise the value is correctly rounded.
[[nodiscard]] inline std::size_t format_mariadb_fixed(
    mariadb_text_buffer& buffer, double value, unsigned int decimals) noexcept {
    namespace text = mariadb_text_detail;
    if (!std::isfinite(value)) {
        // As my_gcvt(), my_fcvt() writes dtoa's overflow as "0".
        buffer[0] = '0';
        return 1;
    }
    const double magnitude = std::fabs(value);
    if (magnitude != 0) {
        const auto found = text::significant(magnitude, 17);
        const int fraction = found.length_ > found.point_ ? found.length_ - found.point_ : 0;
        if (fraction <= static_cast<int>(decimals)) {
            char* out = buffer.data();
            if (std::signbit(value)) {
                *out++ = '-';
            }
            out = text::put_plain(out, found);
            if (decimals > 0) {
                if (fraction == 0) {
                    *out++ = '.';
                }
                for (int pad = static_cast<int>(decimals) - fraction; pad > 0; --pad) {
                    *out++ = '0';
                }
            }
            return static_cast<std::size_t>(out - buffer.data());
        }
    }
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, static_cast<int>(decimals));
    return static_cast<std::size_t>(converted.ptr - buffer.data());
}

// An integer as the text protocol sends it: a ZEROFILL column pads it with
// zeros to the column's display width.
template <typename integer_type>
[[nodiscard]] std::size_t format_mariadb_integer(
    mariadb_text_buffer& buffer, integer_type value, std::size_t zerofill_width) noexcept {
    std::array<char, 24> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    const auto length = static_cast<std::size_t>(converted.ptr - digits.data());
    char* out = buffer.data();
    for (std::size_t pad = length; pad < zerofill_width && pad < buffer.size() - digits.size(); ++pad) {
        *out++ = '0';
    }
    for (std::size_t index = 0; index < length; ++index) {
        *out++ = digits[index];
    }
    return static_cast<std::size_t>(out - buffer.data());
}

// The parts of a DATE, DATETIME, TIMESTAMP or TIME value.
struct mariadb_temporal final {
    unsigned int year_{0};
    unsigned int month_{0};
    unsigned int day_{0};
    // A TIME's hours include its days.
    unsigned int hour_{0};
    unsigned int minute_{0};
    unsigned int second_{0};
    unsigned long microsecond_{0};
    bool negative_{false};
};

enum class mariadb_temporal_kind : std::uint8_t { date,
    datetime,
    time };

// "YYYY-MM-DD", "YYYY-MM-DD HH:MM:SS" or "[-]HH:MM:SS", with the column's
// fractional digits. A column without a fixed number of them
// (AUTO_SEC_PART_DIGITS) shows six when the value has a fraction, else none.
[[nodiscard]] inline std::size_t format_mariadb_temporal(mariadb_text_buffer& buffer, const mariadb_temporal& value,
    mariadb_temporal_kind kind, unsigned int decimals) noexcept {
    if (decimals > 6) {
        decimals = value.microsecond_ != 0 ? 6 : 0;
    }
    char* out = buffer.data();
    const auto put_number = [&out](unsigned long number, int width) {
        for (int index = width - 1; index >= 0; --index) {
            out[index] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
        out += width;
    };
    if (kind != mariadb_temporal_kind::time) {
        put_number(value.year_, 4);
        *out++ = '-';
        put_number(value.month_, 2);
        *out++ = '-';
        put_number(value.day_, 2);
        if (kind == mariadb_temporal_kind::date) {
            return static_cast<std::size_t>(out - buffer.data());
        }
        *out++ = ' ';
        put_number(value.hour_, 2);
    } else {
        if (value.negative_) {
            *out++ = '-';
        }
        put_number(value.hour_, value.hour_ >= 100 ? 3 : 2);
    }
    *out++ = ':';
    put_number(value.minute_, 2);
    *out++ = ':';
    put_number(value.second_, 2);
    if (decimals > 0) {
        *out++ = '.';
        unsigned long fraction = value.microsecond_;
        for (unsigned int drop = 6; drop > decimals; --drop) {
            fraction /= 10;
        }
        put_number(fraction, static_cast<int>(decimals));
    }
    return static_cast<std::size_t>(out - buffer.data());
}

}  // namespace ruvia::detail

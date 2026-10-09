#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

namespace ruvia {

enum class decimal_parse_error { invalid_format,
    out_of_range };

// Explicit inf/nan tokens are values; overflow or underflow of a finite
// decimal is an error. Callers decide whether non-finite values are allowed.
template <std::floating_point t_type = double>
    requires std::same_as<t_type, std::remove_cv_t<t_type>>
[[nodiscard]] std::variant<t_type, decimal_parse_error> parse_decimal_number(std::string_view text) noexcept {
    if (text == "inf" || text == "infinity") {
        return std::numeric_limits<t_type>::infinity();
    }
    if (text == "-inf" || text == "-infinity") {
        return -std::numeric_limits<t_type>::infinity();
    }
    if (text == "nan" || text == "-nan") {
        return std::numeric_limits<t_type>::quiet_NaN();
    }

    // Keep the accepted grammar narrower than from_chars: no leading '+',
    // whitespace, empty fraction, NaN payloads, or other non-finite spellings.
    const bool negative = text.starts_with('-');
    std::size_t index = negative ? 1 : 0;
    bool saw_non_zero = false;
    const auto consume_digits = [&] {
        const auto begin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            saw_non_zero = saw_non_zero || text[index] != '0';
            ++index;
        }
        return index - begin;
    };
    const auto integral_digits = consume_digits();
    std::size_t fractional_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        fractional_digits = consume_digits();
        if (fractional_digits == 0) {
            return decimal_parse_error::invalid_format;
        }
    }
    if (integral_digits == 0 && fractional_digits == 0) {
        return decimal_parse_error::invalid_format;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            ++index;
        }
        const auto exponent_begin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
        if (index == exponent_begin) {
            return decimal_parse_error::invalid_format;
        }
    }
    if (index != text.size()) {
        return decimal_parse_error::invalid_format;
    }
    if (!saw_non_zero) {
        return negative ? -t_type{0} : t_type{0};
    }

    t_type value = 0;
    const auto* end = text.data() + text.size();
    const auto converted = std::from_chars(text.data(), end, value, std::chars_format::general);
    if (converted.ec == std::errc::result_out_of_range) {
        return decimal_parse_error::out_of_range;
    }
    if (converted.ec != std::errc{} || converted.ptr != end) {
        return decimal_parse_error::invalid_format;
    }
    return value;
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::decimal_parse_error;
using ::ruvia::parse_decimal_number;
}  // namespace ruvia::detail

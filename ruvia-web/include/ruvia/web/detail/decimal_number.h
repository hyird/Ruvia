#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia::detail {

// Converts a decimal whose grammar the caller has already validated (optional
// '-', digits with an optional non-empty fraction, optional exponent; no '+',
// whitespace or non-finite spellings). An all-zero significand is a signed
// zero whatever its exponent; otherwise overflow or underflow is an error.
template <std::floating_point t_type>
    requires std::same_as<t_type, std::remove_cv_t<t_type>>
[[nodiscard]] std::optional<t_type> convert_decimal_number(
    std::string_view validated, bool nonzero_significand) noexcept {
    if (!nonzero_significand) {
        return validated.starts_with('-') ? -t_type{0} : t_type{0};
    }
    t_type value = 0;
    const auto* end = validated.data() + validated.size();
    const auto converted = std::from_chars(validated.data(), end, value, std::chars_format::general);
    if (converted.ec != std::errc{} || converted.ptr != end) {
        return std::nullopt;
    }
    return value;
}

// Overflow or underflow of a finite decimal is an error.
template <std::floating_point t_type = double>
    requires std::same_as<t_type, std::remove_cv_t<t_type>>
[[nodiscard]] std::optional<t_type> parse_decimal_number(std::string_view text) noexcept {
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
            return std::nullopt;
        }
    }
    if (integral_digits == 0 && fractional_digits == 0) {
        return std::nullopt;
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
            return std::nullopt;
        }
    }
    if (index != text.size()) {
        return std::nullopt;
    }
    return convert_decimal_number<t_type>(text, saw_non_zero);
}

}  // namespace ruvia::detail

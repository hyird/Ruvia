#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia {

enum class DecimalParseError { kInvalidFormat,
    kOutOfRange };

// Explicit inf/nan tokens are values; overflow or underflow of a finite
// decimal is an error. Callers decide whether non-finite values are allowed.
template <std::floating_point T = double>
    requires std::same_as<T, std::remove_cv_t<T>>
[[nodiscard]] std::expected<T, DecimalParseError> parseDecimalNumber(std::string_view text) noexcept {
    if (text == "inf" || text == "infinity") {
        return std::numeric_limits<T>::infinity();
    }
    if (text == "-inf" || text == "-infinity") {
        return -std::numeric_limits<T>::infinity();
    }
    if (text == "nan" || text == "-nan") {
        return std::numeric_limits<T>::quiet_NaN();
    }

    // Keep the accepted grammar narrower than from_chars: no leading '+',
    // whitespace, empty fraction, NaN payloads, or other non-finite spellings.
    const bool negative = text.starts_with('-');
    std::size_t index = negative ? 1 : 0;
    bool sawNonZero = false;
    const auto consumeDigits = [&] {
        const auto begin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            sawNonZero = sawNonZero || text[index] != '0';
            ++index;
        }
        return index - begin;
    };
    const auto integralDigits = consumeDigits();
    std::size_t fractionalDigits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        fractionalDigits = consumeDigits();
        if (fractionalDigits == 0) {
            return std::unexpected(DecimalParseError::kInvalidFormat);
        }
    }
    if (integralDigits == 0 && fractionalDigits == 0) {
        return std::unexpected(DecimalParseError::kInvalidFormat);
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            ++index;
        }
        const auto exponentBegin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
        if (index == exponentBegin) {
            return std::unexpected(DecimalParseError::kInvalidFormat);
        }
    }
    if (index != text.size()) {
        return std::unexpected(DecimalParseError::kInvalidFormat);
    }
    if (!sawNonZero) {
        return negative ? -T{0} : T{0};
    }

    T value = 0;
    const auto* end = text.data() + text.size();
    const auto converted = std::from_chars(text.data(), end, value, std::chars_format::general);
    if (converted.ec == std::errc::result_out_of_range) {
        return std::unexpected(DecimalParseError::kOutOfRange);
    }
    if (converted.ec != std::errc{} || converted.ptr != end) {
        return std::unexpected(DecimalParseError::kInvalidFormat);
    }
    return value;
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::DecimalParseError;
using ::ruvia::parseDecimalNumber;
}  // namespace ruvia::detail

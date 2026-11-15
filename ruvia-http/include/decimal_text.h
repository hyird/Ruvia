#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia {

namespace detail {
template <typename t_type>
concept parsed_integer = std::integral<t_type> && !std::same_as<std::remove_cv_t<t_type>, bool> && !std::same_as<std::remove_cv_t<t_type>, char> && !std::same_as<std::remove_cv_t<t_type>, wchar_t> && !std::same_as<std::remove_cv_t<t_type>, char8_t> && !std::same_as<std::remove_cv_t<t_type>, char16_t> && !std::same_as<std::remove_cv_t<t_type>, char32_t>;
}  // namespace detail

// The single strict decimal integer parser: no value for empty input,
// whitespace, a leading '+', trailing characters or a value out of range. A
// leading '-' is accepted only for signed integers, so an unsigned result is
// exactly 1*DIGIT.
template <detail::parsed_integer t_type>
[[nodiscard]] constexpr std::optional<t_type> parse_integer(std::string_view text) noexcept {
    t_type value{};
    const auto* last = text.data() + text.size();
    const auto [end, error] = std::from_chars(text.data(), last, value, 10);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

// The single saturating addition for sizes, counters and durations: the sum,
// or the type's maximum when the sum would wrap.
template <std::unsigned_integral value_type>
[[nodiscard]] constexpr value_type saturating_add(value_type left, value_type right) noexcept {
    constexpr auto maximum = (std::numeric_limits<value_type>::max)();
    return left > maximum - right ? maximum : static_cast<value_type>(left + right);
}

// Appends one decimal digit (0-9) to `value`, which must not exceed
// `maximum`; the result saturates at `maximum`. Streaming parsers that cannot
// hand the whole number to parse_saturating_decimal() step with this.
[[nodiscard]] constexpr std::uint64_t accumulate_decimal_digit(std::uint64_t value, unsigned digit, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept {
    return digit > maximum || value > (maximum - digit) / 10 ? maximum : value * 10 + digit;
}

// The single parser for 1*DIGIT fields whose overflow clamps to a bound
// (delta-seconds, Max-Forwards, byte-range positions): no value for empty
// input or any non-digit byte, otherwise the number saturated at `maximum`.
[[nodiscard]] constexpr std::optional<std::uint64_t> parse_saturating_decimal(std::string_view digits, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = accumulate_decimal_digit(value, static_cast<unsigned>(c - '0'), maximum);
    }
    return value;
}

// Longest decimal form of a 64-bit integer: the 20 digits of the largest
// std::uint64_t (18446744073709551615), or the sign plus the 19 digits of the
// most negative std::int64_t.
inline constexpr std::size_t max_decimal_size = 20;

[[nodiscard]] constexpr std::size_t decimal_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

// The single integer formatter: writes the decimal form of `value` at the start
// of `output` and returns that prefix. Signed types keep their sign; every
// width is widened to 64 bits first. Writers that measured an exact slice with
// decimal_size() copy the result instead of formatting in place, so no pointer
// past the slice is ever formed.
template <std::integral integer_type>
[[nodiscard]] inline std::string_view format_decimal(std::span<char, max_decimal_size> output, integer_type value) noexcept {
    char* end = nullptr;
    if constexpr (std::signed_integral<integer_type>) {
        end = std::to_chars(output.data(), output.data() + output.size(), static_cast<std::int64_t>(value)).ptr;
    } else {
        end = std::to_chars(output.data(), output.data() + output.size(), static_cast<std::uint64_t>(value)).ptr;
    }
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

template <std::integral integer_type>
inline void append_decimal(std::string& output, integer_type value) {
    std::array<char, max_decimal_size> digits;
    output.append(format_decimal(digits, value));
}

// Append the shortest decimal form of an arithmetic value; `error_message` is
// thrown as std::logic_error if formatting fails. Integers go through
// append_decimal(), which cannot fail.
template <typename number_type>
void append_number(std::string& output, number_type value, [[maybe_unused]] const char* error_message) {
    if constexpr (std::is_integral_v<number_type>) {
        append_decimal(output, value);
    } else {
        std::array<char, 64> buffer;
        const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (ec != std::errc{}) {
            throw std::logic_error(error_message);
        }
        output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }
}

// Floating-point variant that rejects NaN and infinities with
// std::invalid_argument before touching `output`.
template <typename number_type>
void append_finite_number(std::string& output, number_type value, const char* finite_error_message, const char* format_error_message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(finite_error_message);
    }
    append_number(output, value, format_error_message);
}

}  // namespace ruvia

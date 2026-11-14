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

// The single saturating addition for sizes, counters and durations: the sum,
// or the type's maximum when the sum would wrap.
template <std::unsigned_integral value_type>
[[nodiscard]] constexpr value_type saturating_add(value_type left, value_type right) noexcept {
    constexpr auto maximum = (std::numeric_limits<value_type>::max)();
    return left > maximum - right ? maximum : static_cast<value_type>(left + right);
}

// Appends one decimal digit (0-9) to `value`, which must not exceed
// `maximum`; the result saturates at `maximum`. Streaming parsers that cannot
// hand the whole number to http_parse_saturating_decimal() step with this.
[[nodiscard]] constexpr std::uint64_t http_accumulate_decimal_digit(std::uint64_t value, unsigned digit, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept {
    return digit > maximum || value > (maximum - digit) / 10 ? maximum : value * 10 + digit;
}

// The single parser for 1*DIGIT fields whose overflow clamps to a bound
// (delta-seconds, Max-Forwards, byte-range positions): no value for empty
// input or any non-digit byte, otherwise the number saturated at `maximum`.
[[nodiscard]] constexpr std::optional<std::uint64_t> http_parse_saturating_decimal(std::string_view digits, std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)()) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = http_accumulate_decimal_digit(value, static_cast<unsigned>(c - '0'), maximum);
    }
    return value;
}

// Maximum decimal digits of a std::uint64_t (18446744073709551615).
inline constexpr std::size_t http_max_unsigned_decimal_size = 20;

[[nodiscard]] constexpr std::size_t http_unsigned_decimal_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

// The single integer formatter: writes the decimal form of `value` at the start
// of `output` and returns that prefix. Writers that measured an exact slice
// with http_unsigned_decimal_size() copy the result instead of formatting in
// place, so no pointer past the slice is ever formed.
[[nodiscard]] inline std::string_view http_format_unsigned(std::span<char, http_max_unsigned_decimal_size> output, std::uint64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

// Sign plus the 19 digits of the most negative std::int64_t.
inline constexpr std::size_t http_max_signed_decimal_size = 20;

[[nodiscard]] inline std::string_view http_format_signed(std::span<char, http_max_signed_decimal_size> output, std::int64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

inline void http_append_unsigned(std::string& output, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    output.append(http_format_unsigned(digits, value));
}

inline void http_append_signed(std::string& output, std::int64_t value) {
    std::array<char, http_max_signed_decimal_size> digits;
    output.append(http_format_signed(digits, value));
}

// Append the shortest decimal form of an arithmetic value; `error_message` is
// thrown as std::logic_error if formatting fails. Integers go through
// http_append_signed() / http_append_unsigned(), which cannot fail.
template <typename number_type>
void http_append_formatted_number(std::string& output, number_type value, [[maybe_unused]] const char* error_message) {
    if constexpr (std::is_integral_v<number_type> && std::is_signed_v<number_type>) {
        http_append_signed(output, value);
    } else if constexpr (std::is_integral_v<number_type>) {
        http_append_unsigned(output, value);
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
void http_append_formatted_finite_number(std::string& output, number_type value, const char* finite_error_message, const char* format_error_message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(finite_error_message);
    }
    http_append_formatted_number(output, value, format_error_message);
}

}  // namespace ruvia

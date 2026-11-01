#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace ruvia {

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
[[nodiscard]] inline std::string_view http_format_unsigned(
    std::span<char, http_max_unsigned_decimal_size> output, std::uint64_t value) noexcept {
    const auto end = std::to_chars(output.data(), output.data() + output.size(), value).ptr;
    return {output.data(), static_cast<std::size_t>(end - output.data())};
}

inline void http_append_unsigned(std::pmr::string& output, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    output.append(http_format_unsigned(digits, value));
}

inline void http_append_signed(std::pmr::string& output, std::int64_t value) {
    // Sign plus the 19 digits of the most negative std::int64_t.
    std::array<char, 20> digits;
    const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    output.append(digits.data(), static_cast<std::size_t>(end - digits.data()));
}

}  // namespace ruvia

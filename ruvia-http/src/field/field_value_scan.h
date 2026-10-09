#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// The first input byte occupies the low byte, independent of native byte order.
// The caller must provide at least eight readable bytes.
[[nodiscard]] inline std::uint64_t load_http_field_value_word(const char* data) noexcept {
    std::uint64_t bytes;
    std::memcpy(&bytes, data, sizeof(bytes));
    if constexpr (std::endian::native == std::endian::big) {
        bytes = std::byteswap(bytes);
    }
    return bytes;
}

// The first set high bit identifies the first invalid input byte. DEL detection
// can also mark later bytes, but never changes the position of its first match.
[[nodiscard]] inline std::uint64_t invalid_http_field_value_mask(std::uint64_t bytes) noexcept {
    constexpr std::uint64_t ones = 0x0101010101010101ULL;
    constexpr std::uint64_t high_bits = ones * 0x80U;
    constexpr std::uint64_t low_bits = ones * 0x7fU;
    const auto del = bytes ^ low_bits;
    // Setting each high bit prevents inter-byte borrow when testing < SP.
    // Exclude original high-bit bytes so obs-text remains valid.
    const auto controls = ~((bytes | high_bits) - ones * 0x20U) & ~bytes;
    const auto special = (controls | ((del - ones) & ~del)) & high_bits;
    if (special == 0) {
        return 0;
    }
    // This exact HTAB mask cannot carry between bytes or hide a DEL match.
    const auto tabs = bytes ^ (ones * 9U);
    const auto tab_bits = ~(((tabs & low_bits) + low_bits) | tabs | low_bits);
    return special & ~tab_bits;
}

[[nodiscard]] inline std::size_t http_field_value_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= sizeof(std::uint64_t)) {
        const auto bytes = load_http_field_value_word(value.data() + index);
        if (const auto invalid = invalid_http_field_value_mask(bytes); invalid != 0) {
            return index + static_cast<std::size_t>(std::countr_zero(invalid)) / 8;
        }
        index += sizeof(bytes);
    }
    while (index < value.size() && isHttpFieldValueChar(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

}  // namespace ruvia::detail

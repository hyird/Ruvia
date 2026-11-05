#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

struct utf8_sequence final {
    // Bytes of the well-formed sequence, or of the maximal subpart (Unicode 3.9,
    // definition D93b) that precedes the first ill-formed byte; always >= 1.
    std::size_t length_;
    bool well_formed_;
};

// Classifies the UTF-8 sequence beginning at input[i] (whose lead byte is >= 0x80)
// under the Unicode 3.9 Table 3-7 constraints: no overlong forms (C0/C1, E0 80-9F,
// F0 80-8F), no UTF-16 surrogate code points (ED A0-BF), nothing above U+10FFFF
// (F4 90-.., F5+), and no stray or truncated continuation bytes. The single owner
// of UTF-8 well-formedness: WebSocket text and close reasons, SSE fields, JSON
// parsing (rejects), JSON writing (one U+FFFD per maximal subpart), compile-time
// model wire names, JWT names and MariaDB identifier character counts.
[[nodiscard]] constexpr utf8_sequence scan_utf8_sequence(std::string_view input, std::size_t i) noexcept {
    const auto lead = static_cast<unsigned char>(input[i]);
    std::size_t trail = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trail = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trail = 2;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trail = 3;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return {1, false};  // 0x80-0xC1 (bare continuation / overlong lead) or 0xF5-0xFF
    }
    for (std::size_t k = 1; k <= trail; ++k) {
        if (i + k >= input.size()) {
            return {k, false};
        }
        const auto byte = static_cast<unsigned char>(input[i + k]);
        if (byte < low || byte > high) {
            return {k, false};
        }
        low = 0x80;
        high = 0xBF;
    }
    return {trail + 1, true};
}

[[nodiscard]] constexpr bool is_valid_utf8(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size();) {
        if (static_cast<unsigned char>(value[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto sequence = scan_utf8_sequence(value, i);
        if (!sequence.well_formed_) {
            return false;
        }
        i += sequence.length_;
    }
    return true;
}

}  // namespace ruvia::detail

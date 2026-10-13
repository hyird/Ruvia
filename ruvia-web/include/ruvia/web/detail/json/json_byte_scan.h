#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RUVIA_JSON_SCAN_SSE2 1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define RUVIA_JSON_SCAN_NEON 1
#endif

namespace ruvia::detail {

enum class json_byte_scan_kind : std::uint8_t {
    // Output escaping only treats quotes, backslashes, and controls as special.
    escape,
    // Input string parsing must additionally stop at non-ASCII bytes to validate
    // their UTF-8 sequence before continuing.
    string_token,
};

template <json_byte_scan_kind kind>
[[nodiscard]] inline std::size_t find_json_special_byte(
    std::string_view input, std::size_t offset) noexcept {
    if (offset >= input.size()) {
        return std::string_view::npos;
    }

#if defined(RUVIA_JSON_SCAN_SSE2)
    const auto quote = _mm_set1_epi8('"');
    const auto backslash = _mm_set1_epi8('\\');
    const auto sign_bit = _mm_set1_epi8(static_cast<char>(-128));
    const auto control_limit = _mm_set1_epi8(static_cast<char>(-96));
    while (offset + 16 <= input.size()) {
        const auto bytes_value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input.data() + offset));
        const auto unsigned_bytes = _mm_xor_si128(bytes_value, sign_bit);
        auto special = _mm_or_si128(_mm_cmpeq_epi8(bytes_value, quote), _mm_cmpeq_epi8(bytes_value, backslash));
        special = _mm_or_si128(special, _mm_cmplt_epi8(unsigned_bytes, control_limit));
        auto mask = static_cast<unsigned>(_mm_movemask_epi8(special));
        if constexpr (kind == json_byte_scan_kind::string_token) {
            mask |= static_cast<unsigned>(_mm_movemask_epi8(bytes_value));
        }
        if (mask != 0) {
            return offset + std::countr_zero(mask);
        }
        offset += 16;
    }
#elif defined(RUVIA_JSON_SCAN_NEON)
    const auto quote = vdupq_n_u8(static_cast<std::uint8_t>('"'));
    const auto backslash = vdupq_n_u8(static_cast<std::uint8_t>('\\'));
    const auto control_limit = vdupq_n_u8(0x20);
    const auto high_bit_limit = vdupq_n_u8(0x80);
    while (offset + 16 <= input.size()) {
        const auto bytes_value = vld1q_u8(reinterpret_cast<const std::uint8_t*>(input.data() + offset));
        auto special = vorrq_u8(vceqq_u8(bytes_value, quote), vceqq_u8(bytes_value, backslash));
        special = vorrq_u8(special, vcltq_u8(bytes_value, control_limit));
        if constexpr (kind == json_byte_scan_kind::string_token) {
            special = vorrq_u8(special, vcgeq_u8(bytes_value, high_bit_limit));
        }
        if (vmaxvq_u8(special) != 0) {
            for (std::size_t index = 0; index < 16; ++index) {
                const auto byte = static_cast<unsigned char>(input[offset + index]);
                if (byte == '"' || byte == '\\' || byte < 0x20 ||
                    (kind == json_byte_scan_kind::string_token && byte >= 0x80)) {
                    return offset + index;
                }
            }
        }
        offset += 16;
    }
#endif

    for (; offset < input.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(input[offset]);
        if (byte == '"' || byte == '\\' || byte < 0x20 ||
            (kind == json_byte_scan_kind::string_token && byte >= 0x80)) {
            return offset;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::size_t find_json_escape_byte(
    std::string_view input, std::size_t offset) noexcept {
    return find_json_special_byte<json_byte_scan_kind::escape>(input, offset);
}

[[nodiscard]] inline std::size_t find_json_string_token_byte(
    std::string_view input, std::size_t offset) noexcept {
    return find_json_special_byte<json_byte_scan_kind::string_token>(input, offset);
}

struct json_utf8_sequence final {
    // Bytes of the well-formed sequence, or of the maximal subpart (Unicode 3.9,
    // definition D93b) that precedes the first ill-formed byte; always >= 1.
    std::size_t length_;
    bool well_formed_;
};

// Classifies the UTF-8 sequence beginning at input[i] (whose lead byte is >= 0x80)
// under the Unicode 3.9 Table 3-7 constraints: no overlong forms (C0/C1, E0 80-9F,
// F0 80-8F), no UTF-16 surrogate code points (ED A0-BF), nothing above U+10FFFF
// (F4 90-.., F5+), and no stray or truncated continuation bytes. The single owner
// of UTF-8 well-formedness for JSON parsing (which rejects ill-formed input), JSON
// writing (which substitutes one U+FFFD per maximal subpart) and MariaDB
// identifier length checks (which count characters).
[[nodiscard]] inline json_utf8_sequence scan_json_utf8_sequence(std::string_view input, std::size_t i) noexcept {
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

}  // namespace ruvia::detail

#if defined(RUVIA_JSON_SCAN_SSE2)
#undef RUVIA_JSON_SCAN_SSE2
#elif defined(RUVIA_JSON_SCAN_NEON)
#undef RUVIA_JSON_SCAN_NEON
#endif

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
    std::string_view input, std::size_t offset = 0) noexcept {
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
    std::string_view input, std::size_t offset = 0) noexcept {
    return find_json_special_byte<json_byte_scan_kind::escape>(input, offset);
}

[[nodiscard]] inline std::size_t find_json_string_token_byte(
    std::string_view input, std::size_t offset = 0) noexcept {
    return find_json_special_byte<json_byte_scan_kind::string_token>(input, offset);
}

}  // namespace ruvia::detail

#if defined(RUVIA_JSON_SCAN_SSE2)
#undef RUVIA_JSON_SCAN_SSE2
#elif defined(RUVIA_JSON_SCAN_NEON)
#undef RUVIA_JSON_SCAN_NEON
#endif

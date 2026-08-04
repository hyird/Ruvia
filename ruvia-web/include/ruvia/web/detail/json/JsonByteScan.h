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

enum class JsonByteScanKind : std::uint8_t {
    // Output escaping only treats quotes, backslashes, and controls as special.
    kEscape,
    // Input string parsing must additionally stop at non-ASCII bytes to validate
    // their UTF-8 sequence before continuing.
    kStringToken,
};

template <JsonByteScanKind Kind>
[[nodiscard]] inline std::size_t findJsonSpecialByte(std::string_view input, std::size_t offset = 0) noexcept {
    if (offset >= input.size()) {
        return std::string_view::npos;
    }

#if defined(RUVIA_JSON_SCAN_SSE2)
    const auto quote = _mm_set1_epi8('"');
    const auto backslash = _mm_set1_epi8('\\');
    const auto signBit = _mm_set1_epi8(static_cast<char>(-128));
    const auto controlLimit = _mm_set1_epi8(static_cast<char>(-96));
    while (offset + 16 <= input.size()) {
        const auto bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input.data() + offset));
        const auto unsignedBytes = _mm_xor_si128(bytes, signBit);
        auto special = _mm_or_si128(_mm_cmpeq_epi8(bytes, quote), _mm_cmpeq_epi8(bytes, backslash));
        special = _mm_or_si128(special, _mm_cmplt_epi8(unsignedBytes, controlLimit));
        auto mask = static_cast<unsigned>(_mm_movemask_epi8(special));
        if constexpr (Kind == JsonByteScanKind::kStringToken) {
            mask |= static_cast<unsigned>(_mm_movemask_epi8(bytes));
        }
        if (mask != 0) {
            return offset + std::countr_zero(mask);
        }
        offset += 16;
    }
#elif defined(RUVIA_JSON_SCAN_NEON)
    const auto quote = vdupq_n_u8(static_cast<std::uint8_t>('"'));
    const auto backslash = vdupq_n_u8(static_cast<std::uint8_t>('\\'));
    const auto controlLimit = vdupq_n_u8(0x20);
    const auto highBitLimit = vdupq_n_u8(0x80);
    while (offset + 16 <= input.size()) {
        const auto bytes = vld1q_u8(reinterpret_cast<const std::uint8_t*>(input.data() + offset));
        auto special = vorrq_u8(vceqq_u8(bytes, quote), vceqq_u8(bytes, backslash));
        special = vorrq_u8(special, vcltq_u8(bytes, controlLimit));
        if constexpr (Kind == JsonByteScanKind::kStringToken) {
            special = vorrq_u8(special, vcgeq_u8(bytes, highBitLimit));
        }
        if (vmaxvq_u8(special) != 0) {
            for (std::size_t index = 0; index < 16; ++index) {
                const auto byte = static_cast<unsigned char>(input[offset + index]);
                if (byte == '"' || byte == '\\' || byte < 0x20 || (Kind == JsonByteScanKind::kStringToken && byte >= 0x80)) {
                    return offset + index;
                }
            }
        }
        offset += 16;
    }
#endif

    for (; offset < input.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(input[offset]);
        if (byte == '"' || byte == '\\' || byte < 0x20 || (Kind == JsonByteScanKind::kStringToken && byte >= 0x80)) {
            return offset;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::size_t findJsonEscapeByte(std::string_view input, std::size_t offset = 0) noexcept {
    return findJsonSpecialByte<JsonByteScanKind::kEscape>(input, offset);
}

[[nodiscard]] inline std::size_t findJsonStringTokenByte(std::string_view input, std::size_t offset = 0) noexcept {
    return findJsonSpecialByte<JsonByteScanKind::kStringToken>(input, offset);
}

}  // namespace ruvia::detail

#if defined(RUVIA_JSON_SCAN_SSE2)
#undef RUVIA_JSON_SCAN_SSE2
#elif defined(RUVIA_JSON_SCAN_NEON)
#undef RUVIA_JSON_SCAN_NEON
#endif

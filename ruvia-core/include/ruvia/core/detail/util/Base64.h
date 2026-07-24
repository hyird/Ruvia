#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ruvia::detail {

[[nodiscard]] inline constexpr std::size_t base64EncodedSize(std::size_t inputSize) noexcept {
    return 4 * ((inputSize + 2) / 3);
}

// Standard-alphabet base64 with '=' padding. `output` must have room for
// base64EncodedSize(input.size()) characters.
inline void encodeBase64(char* output, std::span<const std::uint8_t> input) noexcept {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t i = 0;
    std::size_t out = 0;
    while (i + 3 <= input.size()) {
        const auto value = (static_cast<std::uint32_t>(input[i]) << 16) | (static_cast<std::uint32_t>(input[i + 1]) << 8) | static_cast<std::uint32_t>(input[i + 2]);
        output[out++] = table[(value >> 18) & 0x3F];
        output[out++] = table[(value >> 12) & 0x3F];
        output[out++] = table[(value >> 6) & 0x3F];
        output[out++] = table[value & 0x3F];
        i += 3;
    }
    if (i == input.size()) {
        return;
    }
    const auto remaining = input.size() - i;
    const auto value = static_cast<std::uint32_t>(input[i]) << 16 | (remaining == 2 ? static_cast<std::uint32_t>(input[i + 1]) << 8 : 0U);
    output[out++] = table[(value >> 18) & 0x3F];
    output[out++] = table[(value >> 12) & 0x3F];
    output[out++] = remaining == 2 ? table[(value >> 6) & 0x3F] : '=';
    output[out] = '=';
}

}  // namespace ruvia::detail

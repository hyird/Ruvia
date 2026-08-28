#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ruvia::detail {

inline void encodeHttpBase64(char* output, std::span<const std::uint8_t> input) noexcept {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t in = 0;
    std::size_t out = 0;
    while (input.size() - in >= 3) {
        const std::uint32_t value = (static_cast<std::uint32_t>(input[in]) << 16) | (static_cast<std::uint32_t>(input[in + 1]) << 8) | static_cast<std::uint32_t>(input[in + 2]);
        output[out++] = kAlphabet[(value >> 18) & 0x3F];
        output[out++] = kAlphabet[(value >> 12) & 0x3F];
        output[out++] = kAlphabet[(value >> 6) & 0x3F];
        output[out++] = kAlphabet[value & 0x3F];
        in += 3;
    }
    const auto remaining = input.size() - in;
    if (remaining == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(input[in]) << 16;
        output[out++] = kAlphabet[(value >> 18) & 0x3F];
        output[out++] = kAlphabet[(value >> 12) & 0x3F];
        output[out++] = '=';
        output[out++] = '=';
    } else if (remaining == 2) {
        const std::uint32_t value = (static_cast<std::uint32_t>(input[in]) << 16) | (static_cast<std::uint32_t>(input[in + 1]) << 8);
        output[out++] = kAlphabet[(value >> 18) & 0x3F];
        output[out++] = kAlphabet[(value >> 12) & 0x3F];
        output[out++] = kAlphabet[(value >> 6) & 0x3F];
        output[out++] = '=';
    }
}

}  // namespace ruvia::detail

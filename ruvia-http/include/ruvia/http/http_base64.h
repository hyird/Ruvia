#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ruvia {

// RFC 4648 section 4 (standard) and section 5 (URL-safe) alphabets.
inline constexpr std::string_view http_base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view http_base64_url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] constexpr std::size_t http_base64_encoded_size(std::size_t input_size) noexcept {
    return 4 * ((input_size + 2) / 3);
}

// Standard-alphabet base64 with '=' padding. `output` must have room for
// http_base64_encoded_size(input.size()) characters.
inline void http_base64_encode(char* output, std::span<const std::uint8_t> input) noexcept {
    std::size_t in = 0;
    std::size_t out = 0;
    while (input.size() - in >= 3) {
        const auto value = (static_cast<std::uint32_t>(input[in]) << 16) |
                           (static_cast<std::uint32_t>(input[in + 1]) << 8) |
                           static_cast<std::uint32_t>(input[in + 2]);
        output[out++] = http_base64_alphabet[(value >> 18) & 0x3F];
        output[out++] = http_base64_alphabet[(value >> 12) & 0x3F];
        output[out++] = http_base64_alphabet[(value >> 6) & 0x3F];
        output[out++] = http_base64_alphabet[value & 0x3F];
        in += 3;
    }
    const auto remaining = input.size() - in;
    if (remaining == 0) {
        return;
    }
    const auto value = (static_cast<std::uint32_t>(input[in]) << 16) |
                       (remaining == 2 ? static_cast<std::uint32_t>(input[in + 1]) << 8 : 0U);
    output[out++] = http_base64_alphabet[(value >> 18) & 0x3F];
    output[out++] = http_base64_alphabet[(value >> 12) & 0x3F];
    output[out++] = remaining == 2 ? http_base64_alphabet[(value >> 6) & 0x3F] : '=';
    output[out] = '=';
}

// Decode one standard-alphabet character to its 6-bit value; returns -1 for
// any other character, including the '=' pad.
[[nodiscard]] constexpr int http_base64_decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// URL-safe counterpart of http_base64_decode_char ('-' and '_' replace '+'
// and '/').
[[nodiscard]] constexpr int http_base64_url_decode_char(char c) noexcept {
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return c == '+' || c == '/' ? -1 : http_base64_decode_char(c);
}

}  // namespace ruvia

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ruvia {

// RFC 4648 section 4 (standard) and section 5 (URL-safe) alphabets.
inline constexpr std::string_view http_base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view http_base64_url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum class http_base64_charset : std::uint8_t {
    standard,  // RFC 4648 section 4: '+' and '/'
    url_safe,  // RFC 4648 section 5: '-' and '_'
};

enum class http_base64_padding : std::uint8_t {
    required,   // '=' completes the final quantum; length is a multiple of 4
    forbidden,  // unpadded (RFC 4648 section 3.2), e.g. JWS base64url
};

// Throws std::length_error when the output length cannot fit std::size_t.
[[nodiscard]] constexpr std::size_t http_base64_encoded_size(
    std::size_t input_size, http_base64_padding padding = http_base64_padding::required) {
    const auto groups = input_size / 3;
    const auto remainder = input_size % 3;
    const std::size_t tail =
        remainder == 0 ? 0 : (padding == http_base64_padding::required ? 4 : remainder + 1);
    if (groups > ((std::numeric_limits<std::size_t>::max)() - tail) / 4) {
        throw std::length_error("base64 output length is not representable");
    }
    return groups * 4 + tail;
}

// The single base64 encoder. `output` must have room for
// http_base64_encoded_size(input.size(), padding) characters.
inline void http_base64_encode(char* output, std::span<const std::uint8_t> input,
    http_base64_charset charset = http_base64_charset::standard,
    http_base64_padding padding = http_base64_padding::required) noexcept {
    const auto alphabet =
        charset == http_base64_charset::url_safe ? http_base64_url_alphabet : http_base64_alphabet;
    std::size_t in = 0;
    std::size_t out = 0;
    while (input.size() - in >= 3) {
        const auto value = (static_cast<std::uint32_t>(input[in]) << 16) |
                           (static_cast<std::uint32_t>(input[in + 1]) << 8) |
                           static_cast<std::uint32_t>(input[in + 2]);
        output[out++] = alphabet[(value >> 18) & 0x3F];
        output[out++] = alphabet[(value >> 12) & 0x3F];
        output[out++] = alphabet[(value >> 6) & 0x3F];
        output[out++] = alphabet[value & 0x3F];
        in += 3;
    }
    const auto remaining = input.size() - in;
    if (remaining == 0) {
        return;
    }
    const auto value = (static_cast<std::uint32_t>(input[in]) << 16) |
                       (remaining == 2 ? static_cast<std::uint32_t>(input[in + 1]) << 8 : 0U);
    output[out++] = alphabet[(value >> 18) & 0x3F];
    output[out++] = alphabet[(value >> 12) & 0x3F];
    if (remaining == 2) {
        output[out++] = alphabet[(value >> 6) & 0x3F];
    } else if (padding == http_base64_padding::required) {
        output[out++] = '=';
    }
    if (padding == http_base64_padding::required) {
        output[out] = '=';
    }
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

// Upper bound of the decoded size of `input_size` characters; exact for valid
// unpadded input.
[[nodiscard]] constexpr std::size_t http_base64_decoded_size_bound(std::size_t input_size) noexcept {
    const auto remainder = input_size % 4;
    return input_size / 4 * 3 + (remainder == 0 ? 0 : remainder - 1);
}

// The single base64 decoder; allocation-free. Accepts only canonical input:
// characters of `charset`, '=' exactly as `padding` requires, no final
// quantum of one character, and zero unused bits in the final quantum
// (RFC 4648 section 3.5). Returns the decoded byte count, or nullopt for
// invalid input or an `output` too small for the decoded bytes. Length and
// ownership policy belongs to the caller.
[[nodiscard]] constexpr std::optional<std::size_t> http_base64_decode(std::string_view input,
    std::span<std::uint8_t> output, http_base64_charset charset, http_base64_padding padding) noexcept {
    auto data = input;
    if (padding == http_base64_padding::required) {
        if (input.size() % 4 != 0) {
            return std::nullopt;
        }
        if (data.ends_with("==")) {
            data.remove_suffix(2);
        } else if (data.ends_with('=')) {
            data.remove_suffix(1);
        }
        // Padding only completes a partial final quantum.
        if (data.size() != input.size() && data.size() % 4 + (input.size() - data.size()) != 4) {
            return std::nullopt;
        }
    }
    const auto remainder = data.size() % 4;
    if (remainder == 1) {
        return std::nullopt;
    }
    const auto decoded_size = http_base64_decoded_size_bound(data.size());
    if (decoded_size > output.size()) {
        return std::nullopt;
    }
    const auto value_of = [charset](char c) noexcept {
        return charset == http_base64_charset::url_safe ? http_base64_url_decode_char(c)
                                                        : http_base64_decode_char(c);
    };
    std::size_t out = 0;
    std::size_t in = 0;
    for (; data.size() - in >= 4; in += 4) {
        const auto first = value_of(data[in]);
        const auto second = value_of(data[in + 1]);
        const auto third = value_of(data[in + 2]);
        const auto fourth = value_of(data[in + 3]);
        if ((first | second | third | fourth) < 0) {
            return std::nullopt;
        }
        output[out++] = static_cast<std::uint8_t>((first << 2) | (second >> 4));
        output[out++] = static_cast<std::uint8_t>((second << 4) | (third >> 2));
        output[out++] = static_cast<std::uint8_t>((third << 6) | fourth);
    }
    if (remainder == 0) {
        return out;
    }
    const auto first = value_of(data[in]);
    const auto second = value_of(data[in + 1]);
    const auto third = remainder == 3 ? value_of(data[in + 2]) : 0;
    if ((first | second | third) < 0 || (remainder == 2 && (second & 0x0F) != 0) ||
        (remainder == 3 && (third & 0x03) != 0)) {
        return std::nullopt;
    }
    output[out++] = static_cast<std::uint8_t>((first << 2) | (second >> 4));
    if (remainder == 3) {
        output[out++] = static_cast<std::uint8_t>((second << 4) | (third >> 2));
    }
    return out;
}

}  // namespace ruvia

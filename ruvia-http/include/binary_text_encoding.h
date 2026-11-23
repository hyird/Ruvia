#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "byte_view.h"

namespace ruvia {

namespace detail {

[[nodiscard]] inline std::size_t forward_encoded_groups(const void* output, const void* input, std::size_t groups) noexcept {
    const auto output_address = reinterpret_cast<std::uintptr_t>(output);
    const auto input_address = reinterpret_cast<std::uintptr_t>(input);
    if (output_address >= input_address) {
        return 0;
    }
    const auto distance = input_address - output_address;
    return distance < groups ? static_cast<std::size_t>(distance) : groups;
}

}  // namespace detail

// Decode one hex digit (either case); returns -1 for a non-hex character.
[[nodiscard]] constexpr int decode_hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Encode the low nibble of `value` as a hex digit. Two case-specific functions
// keep the case selection at the call site: percent-encoding needs uppercase
// (RFC 3986 section 2.1), token/hash encoders conventionally use lowercase.
[[nodiscard]] constexpr char lower_hex_digit(int value) noexcept {
    return "0123456789abcdef"[value & 0x0F];
}

[[nodiscard]] constexpr char upper_hex_digit(int value) noexcept {
    return "0123456789ABCDEF"[value & 0x0F];
}

// Allocation-free bulk decoding. Requires an even digit count and enough
// output space. Invalid input may leave a decoded prefix in output. In-place
// decoding is supported; output beginning inside unread input is rejected.
[[nodiscard]] constexpr std::optional<std::size_t> decode_hex(std::string_view input, mutable_byte_view output) noexcept {
    if (input.size() % 2 != 0 || input.size() / 2 > output.size()) {
        return std::nullopt;
    }
    if (!std::is_constant_evaluated() && !input.empty()) {
        const auto first = reinterpret_cast<std::uintptr_t>(input.data());
        const auto target = reinterpret_cast<std::uintptr_t>(output.data());
        if (target > first && target - first < input.size()) {
            return std::nullopt;
        }
    }
    for (std::size_t i = 0; i < input.size() / 2; ++i) {
        const auto high = decode_hex_nibble(input[2 * i]);
        const auto low = decode_hex_nibble(input[2 * i + 1]);
        if ((high | low) < 0) {
            return std::nullopt;
        }
        output[i] = static_cast<std::byte>((high << 4) | low);
    }
    return input.size() / 2;
}

// The required length counts hex digits, not decoded bytes.
[[nodiscard]] constexpr bool is_lower_hex(std::string_view input, std::size_t required_length) noexcept {
    if (input.size() != required_length) {
        return false;
    }
    for (const char c : input) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

namespace detail {

template <bool uppercase>
constexpr std::string_view encode_hex_into(std::span<char> output RUVIA_LIFETIMEBOUND, byte_view bytes) {
    if (bytes.size() > output.size() / 2) {
        throw std::invalid_argument("hex output buffer is too small");
    }
    char* const dst = output.data();
    const auto encode_byte = [dst, bytes](std::size_t index) noexcept {
        const auto byte = std::to_integer<unsigned int>(bytes[index]);
        if constexpr (uppercase) {
            dst[2 * index] = upper_hex_digit(static_cast<int>(byte >> 4));
            dst[2 * index + 1] = upper_hex_digit(static_cast<int>(byte));
        } else {
            dst[2 * index] = lower_hex_digit(static_cast<int>(byte >> 4));
            dst[2 * index + 1] = lower_hex_digit(static_cast<int>(byte));
        }
    };
    auto forward_bytes = bytes.size();
    if (!std::is_constant_evaluated()) {
        forward_bytes = forward_encoded_groups(dst, bytes.data(), bytes.size());
    }
    for (auto index = bytes.size(); index != forward_bytes;) {
        encode_byte(--index);
    }
    for (std::size_t index = 0; index != forward_bytes; ++index) {
        encode_byte(index);
    }
    return {dst, 2 * bytes.size()};
}

template <bool uppercase>
inline void append_hex(std::string& output, byte_view bytes) {
    const auto start = output.size();
    if (bytes.size() > (output.max_size() - start) / 2) {
        throw std::length_error("hex output length is not representable");
    }
    const auto first = reinterpret_cast<std::uintptr_t>(output.data());
    const auto bytes_address = reinterpret_cast<std::uintptr_t>(bytes.data());
    const bool aliased = !bytes.empty() && bytes_address >= first && bytes_address - first <= start;
    const auto offset = aliased ? static_cast<std::size_t>(bytes_address - first) : 0;
    output.resize(start + bytes.size() * 2);
    // An aliased source ends at the latest at the old terminating NUL, index
    // start, which resize rewrote with the same NUL.
    const auto source = aliased ? byte_view(reinterpret_cast<const std::byte*>(output.data() + offset), bytes.size()) : bytes;
    (void)encode_hex_into<uppercase>(std::span<char>(output.data() + start, bytes.size() * 2), source);
}

}  // namespace detail

// Writes 2 * bytes.size() digits at the start of `output` and returns that
// prefix. Output may overlap input anywhere, including in place. Throws
// std::invalid_argument before writing when `output` is too small.
constexpr std::string_view encode_lower_hex(std::span<char> output RUVIA_LIFETIMEBOUND, byte_view bytes) {
    return detail::encode_hex_into<false>(output, bytes);
}

constexpr std::string_view encode_upper_hex(std::span<char> output RUVIA_LIFETIMEBOUND, byte_view bytes) {
    return detail::encode_hex_into<true>(output, bytes);
}

inline std::string_view encode_lower_hex(std::span<char> output RUVIA_LIFETIMEBOUND, std::string_view bytes) {
    return encode_lower_hex(output, as_bytes(bytes));
}

inline std::string_view encode_upper_hex(std::span<char> output RUVIA_LIFETIMEBOUND, std::string_view bytes) {
    return encode_upper_hex(output, as_bytes(bytes));
}

// Input may alias output, including its terminating NUL. Length overflow
// throws std::length_error without modifying output.
inline void append_lower_hex(std::string& output, byte_view bytes) {
    detail::append_hex<false>(output, bytes);
}

inline void append_upper_hex(std::string& output, byte_view bytes) {
    detail::append_hex<true>(output, bytes);
}

inline void append_lower_hex(std::string& output, std::string_view bytes) {
    append_lower_hex(output, as_bytes(bytes));
}

inline void append_upper_hex(std::string& output, std::string_view bytes) {
    append_upper_hex(output, as_bytes(bytes));
}

[[nodiscard]] inline std::string encode_lower_hex(byte_view bytes) {
    std::string out;
    append_lower_hex(out, bytes);
    return out;
}

[[nodiscard]] inline std::string encode_upper_hex(byte_view bytes) {
    std::string out;
    append_upper_hex(out, bytes);
    return out;
}

[[nodiscard]] inline std::string encode_lower_hex(std::string_view bytes) {
    return encode_lower_hex(as_bytes(bytes));
}

[[nodiscard]] inline std::string encode_upper_hex(std::string_view bytes) {
    return encode_upper_hex(as_bytes(bytes));
}

// RFC 4648 section 4 (standard) and section 5 (URL-safe) alphabets.
inline constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr std::string_view base64_url_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum class base64_charset : std::uint8_t {
    standard,  // RFC 4648 section 4: '+' and '/'
    url_safe,  // RFC 4648 section 5: '-' and '_'
    both,      // Decode only: accept either alphabet, including mixed input.
};

enum class base64_padding : std::uint8_t {
    required,   // '=' completes the final quantum; length is a multiple of 4
    forbidden,  // unpadded (RFC 4648 section 3.2), e.g. JWS base64url
    optional,   // Decode only: accept either complete padding or no padding.
};

// Throws std::length_error on length overflow, std::invalid_argument for
// decode-only or invalid padding options.
[[nodiscard]] constexpr std::size_t base64_encoded_size(std::size_t input_size, base64_padding padding = base64_padding::required) {
    if (padding != base64_padding::required && padding != base64_padding::forbidden) {
        throw std::invalid_argument("base64 encoding requires an explicit padding policy");
    }
    const auto groups = input_size / 3;
    const auto remainder = input_size % 3;
    const std::size_t tail = remainder == 0 ? 0 : (padding == base64_padding::required ? 4 : remainder + 1);
    if (groups > ((std::numeric_limits<std::size_t>::max)() - tail) / 4) {
        throw std::length_error("base64 output length is not representable");
    }
    return groups * 4 + tail;
}

// The single base64 encoder. `output` must have room for
// base64_encoded_size(input.size(), padding) characters and may overlap
// `input` anywhere, including in place. Decode-only, invalid options or an
// undersized `output` throw std::invalid_argument before any output is written.
constexpr std::string_view base64_encode(std::span<char> output RUVIA_LIFETIMEBOUND, byte_view input, base64_charset charset = base64_charset::standard, base64_padding padding = base64_padding::required) {
    if ((charset != base64_charset::standard && charset != base64_charset::url_safe) || (padding != base64_padding::required && padding != base64_padding::forbidden)) {
        throw std::invalid_argument("base64 decode-only options cannot encode");
    }
    const auto encoded_size = base64_encoded_size(input.size(), padding);
    if (output.size() < encoded_size) {
        throw std::invalid_argument("base64 output buffer is too small");
    }
    char* const dst = output.data();
    const auto alphabet = charset == base64_charset::url_safe ? base64_url_alphabet : base64_alphabet;
    const bool padded = padding == base64_padding::required;
    // Group k reads input[3k, 3k + 3) before writing dst[4k, 4k + 4).
    const auto encode_group = [dst, input, alphabet, padded](std::size_t group) noexcept {
        const auto bytes = input.subspan(group * 3, (std::min)(input.size() - group * 3, std::size_t{3}));
        auto value = std::to_integer<std::uint32_t>(bytes[0]) << 16;
        if (bytes.size() > 1) {
            value |= std::to_integer<std::uint32_t>(bytes[1]) << 8;
        }
        if (bytes.size() > 2) {
            value |= std::to_integer<std::uint32_t>(bytes[2]);
        }
        auto* const digits = dst + group * 4;
        digits[0] = alphabet[(value >> 18) & 0x3F];
        digits[1] = alphabet[(value >> 12) & 0x3F];
        if (bytes.size() > 1) {
            digits[2] = alphabet[(value >> 6) & 0x3F];
        } else if (padded) {
            digits[2] = '=';
        }
        if (bytes.size() > 2) {
            digits[3] = alphabet[value & 0x3F];
        } else if (padded) {
            digits[3] = '=';
        }
    };
    const auto groups = input.size() / 3 + (input.size() % 3 == 0 ? 0 : 1);
    auto forward_groups = groups;
    if (!std::is_constant_evaluated()) {
        forward_groups = detail::forward_encoded_groups(dst, input.data(), groups);
    }
    for (auto group = groups; group != forward_groups;) {
        encode_group(--group);
    }
    for (std::size_t group = 0; group != forward_groups; ++group) {
        encode_group(group);
    }
    return {dst, encoded_size};
}

inline std::string_view base64_encode(std::span<char> output RUVIA_LIFETIMEBOUND, std::string_view input, base64_charset charset = base64_charset::standard, base64_padding padding = base64_padding::required) {
    return base64_encode(output, as_bytes(input), charset, padding);
}

[[nodiscard]] inline std::string base64_encode(byte_view input, base64_charset charset = base64_charset::standard, base64_padding padding = base64_padding::required) {
    if ((charset != base64_charset::standard && charset != base64_charset::url_safe) || (padding != base64_padding::required && padding != base64_padding::forbidden)) {
        throw std::invalid_argument("base64 decode-only options cannot encode");
    }
    const auto encoded_size = base64_encoded_size(input.size(), padding);
    if (encoded_size == 0) {
        return {};
    }
    std::string out(encoded_size, '\0');
    (void)base64_encode(std::span<char>(out.data(), out.size()), input, charset, padding);
    return out;
}

[[nodiscard]] inline std::string base64_encode(std::string_view input, base64_charset charset = base64_charset::standard, base64_padding padding = base64_padding::required) {
    return base64_encode(as_bytes(input), charset, padding);
}

// Decode one standard-alphabet character to its 6-bit value; returns -1 for
// any other character, including the '=' pad.
[[nodiscard]] constexpr int base64_decode_char(char c) noexcept {
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

// URL-safe counterpart of base64_decode_char ('-' and '_' replace '+' and '/').
[[nodiscard]] constexpr int base64_url_decode_char(char c) noexcept {
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return c == '+' || c == '/' ? -1 : base64_decode_char(c);
}

// Upper bound of the decoded size of `input_size` characters; exact for valid
// unpadded input.
[[nodiscard]] constexpr std::size_t base64_decoded_size_bound(std::size_t input_size) noexcept {
    const auto remainder = input_size % 4;
    return input_size / 4 * 3 + (remainder == 0 ? 0 : remainder - 1);
}

// The single base64 decoder; allocation-free. Accepts only canonical input:
// characters of `charset`, '=' exactly as `padding` requires, no final
// quantum of one character, and zero unused bits in the final quantum
// (RFC 4648 section 3.5). Returns the decoded byte count, or nullopt for
// invalid input or an `output` too small for the decoded bytes. `output` may
// start at or before `input` (in-place decoding) or past its encoded
// characters; one starting inside them, after the first, would overwrite
// unread input and is rejected with nullopt before any write. Length and
// ownership policy belongs to the caller.
[[nodiscard]] constexpr std::optional<std::size_t> base64_decode(std::string_view input, mutable_byte_view output, base64_charset charset = base64_charset::standard, base64_padding padding = base64_padding::required) noexcept {
    if ((charset != base64_charset::standard && charset != base64_charset::url_safe && charset != base64_charset::both) || (padding != base64_padding::required && padding != base64_padding::forbidden && padding != base64_padding::optional)) {
        return std::nullopt;
    }
    auto data = input;
    if (padding == base64_padding::required || (padding == base64_padding::optional && input.ends_with('='))) {
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
    const auto decoded_size = base64_decoded_size_bound(data.size());
    if (decoded_size > output.size()) {
        return std::nullopt;
    }
    // Overlap needs reinterpret_cast, which constant evaluation excludes.
    if (!std::is_constant_evaluated() && !data.empty()) {
        const auto encoded = reinterpret_cast<std::uintptr_t>(data.data());
        const auto output_address = reinterpret_cast<std::uintptr_t>(output.data());
        if (output_address > encoded && output_address - encoded < data.size()) {
            return std::nullopt;
        }
    }
    const auto value_of = [charset](char c) noexcept {
        if (charset == base64_charset::both && (c == '-' || c == '_')) {
            return c == '-' ? 62 : 63;
        }
        return charset == base64_charset::url_safe ? base64_url_decode_char(c) : base64_decode_char(c);
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
        output[out++] = static_cast<std::byte>((first << 2) | (second >> 4));
        output[out++] = static_cast<std::byte>((second << 4) | (third >> 2));
        output[out++] = static_cast<std::byte>((third << 6) | fourth);
    }
    if (remainder == 0) {
        return out;
    }
    const auto first = value_of(data[in]);
    const auto second = value_of(data[in + 1]);
    const auto third = remainder == 3 ? value_of(data[in + 2]) : 0;
    if ((first | second | third) < 0 || (remainder == 2 && (second & 0x0F) != 0) || (remainder == 3 && (third & 0x03) != 0)) {
        return std::nullopt;
    }
    output[out++] = static_cast<std::byte>((first << 2) | (second >> 4));
    if (remainder == 3) {
        output[out++] = static_cast<std::byte>((second << 4) | (third >> 2));
    }
    return out;
}

}  // namespace ruvia

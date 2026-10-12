#pragma once

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ruvia {

// Decode one hex digit (either case); returns -1 for a non-hex character.
[[nodiscard]] constexpr int http_decode_hex_nibble(char c) noexcept {
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
[[nodiscard]] constexpr char http_lower_hex_digit(int value) noexcept {
    return "0123456789abcdef"[value & 0x0F];
}

[[nodiscard]] constexpr char http_upper_hex_digit(int value) noexcept {
    return "0123456789ABCDEF"[value & 0x0F];
}

// The single byte-sequence to lowercase hex encoder: writes exactly
// 2 * bytes.size() digits starting at `output` and returns the end.
constexpr char* http_encode_lower_hex(char* output, std::span<const unsigned char> bytes) noexcept {
    for (const auto byte : bytes) {
        *output++ = http_lower_hex_digit(byte >> 4);
        *output++ = http_lower_hex_digit(byte);
    }
    return output;
}

// Text is encoded byte by byte, like its unsigned char representation.
inline char* http_encode_lower_hex(char* output, std::string_view bytes) noexcept {
    return http_encode_lower_hex(
        output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// `bytes` may alias `output`, up to and including its terminating NUL at
// output.data()[output.size()]; an aliased input is rebased by offset after
// the resize, which may reallocate. Throws std::length_error when the result
// cannot fit the string.
inline void http_append_lower_hex(std::pmr::string& output, std::span<const unsigned char> bytes) {
    const auto start = output.size();
    if (bytes.size() > (output.max_size() - start) / 2) {
        throw std::length_error("hex output length is not representable");
    }
    const auto* const first = reinterpret_cast<const unsigned char*>(output.data());
    const std::less<const unsigned char*> before;
    const bool aliased = !bytes.empty() && !before(bytes.data(), first) && !before(first + start, bytes.data());
    const auto offset = aliased ? static_cast<std::size_t>(bytes.data() - first) : 0;
    output.resize(start + bytes.size() * 2);
    if (!aliased) {
        http_encode_lower_hex(output.data() + start, bytes);
        return;
    }
    // The source ends at the latest at the old terminating NUL, index start,
    // where the digits begin (resize rewrote that byte with the same NUL).
    // Encoding backwards keeps every write above the unread source bytes.
    const auto* const source = reinterpret_cast<const unsigned char*>(output.data()) + offset;
    auto* digit = output.data() + output.size();
    for (auto index = bytes.size(); index != 0;) {
        const auto byte = source[--index];
        *--digit = http_lower_hex_digit(byte);
        *--digit = http_lower_hex_digit(byte >> 4);
    }
}

inline void http_append_lower_hex(std::pmr::string& output, std::string_view bytes) {
    http_append_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

}  // namespace ruvia

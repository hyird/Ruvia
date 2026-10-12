#pragma once

#include <cstddef>
#include <memory_resource>
#include <functional>
#include <span>
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

// `bytes` may alias `output`; an overlapping input is rebased by offset after
// the resize, which may reallocate.
inline void http_append_lower_hex(std::pmr::string& output, std::span<const unsigned char> bytes) {
    const auto start = output.size();
    const auto* const first = reinterpret_cast<const unsigned char*>(output.data());
    const std::less<const unsigned char*> before;
    const bool aliased = !bytes.empty() && !before(bytes.data(), first) && before(bytes.data(), first + start);
    const auto offset = aliased ? static_cast<std::size_t>(bytes.data() - first) : 0;
    output.resize(start + bytes.size() * 2);
    if (!aliased) {
        http_encode_lower_hex(output.data() + start, bytes);
        return;
    }
    // The source lies inside [0, start) and the digits land at [start, ...), so
    // no write can clobber an unread source byte.
    const auto* source = reinterpret_cast<const unsigned char*>(output.data()) + offset;
    http_encode_lower_hex(output.data() + start, std::span(source, bytes.size()));
}

inline void http_append_lower_hex(std::pmr::string& output, std::string_view bytes) {
    http_append_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

}  // namespace ruvia

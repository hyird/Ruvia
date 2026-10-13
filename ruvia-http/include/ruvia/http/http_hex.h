#pragma once

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/util/http_encode_overlap.h"

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
// 2 * bytes.size() digits starting at `output` and returns the end. `output`
// may overlap `bytes` anywhere, including in place.
constexpr char* http_encode_lower_hex(char* output, std::span<const unsigned char> bytes) noexcept {
    const auto encode_byte = [output, bytes](std::size_t index) noexcept {
        const auto byte = bytes[index];
        output[2 * index] = http_lower_hex_digit(byte >> 4);
        output[2 * index + 1] = http_lower_hex_digit(byte);
    };
    // Constant evaluation cannot make char output alias unsigned char input.
    const auto forward_bytes = std::is_constant_evaluated()
                                   ? bytes.size()
                                   : detail::http_forward_encoded_groups(output, bytes.data(), bytes.size());
    for (auto index = bytes.size(); index != forward_bytes;) {
        encode_byte(--index);
    }
    for (std::size_t index = 0; index != forward_bytes; ++index) {
        encode_byte(index);
    }
    return output + 2 * bytes.size();
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
    // An aliased source ends at the latest at the old terminating NUL, index
    // start, which resize rewrote with the same NUL.
    const auto source =
        aliased ? std::span(reinterpret_cast<const unsigned char*>(output.data()) + offset, bytes.size()) : bytes;
    http_encode_lower_hex(output.data() + start, source);
}

inline void http_append_lower_hex(std::pmr::string& output, std::string_view bytes) {
    http_append_lower_hex(output, std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

}  // namespace ruvia

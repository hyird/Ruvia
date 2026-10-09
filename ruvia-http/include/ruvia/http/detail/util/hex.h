#pragma once

namespace ruvia::detail {

[[nodiscard]] inline int decode_hex_nibble(char c) noexcept {
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

// Encode the low nibble of `value` as a hex digit. The counterpart to
// decode_hex_nibble, so both directions of hex conversion live in one place.
// Two case-specific overloads keep the case selection at the call site (a
// compile-time constant) rather than a runtime branch: percent-encoding needs
// uppercase (RFC 3986 §2.1), token/hash encoders conventionally use lowercase.
[[nodiscard]] inline char lower_hex_digit(int value) noexcept {
    return "0123456789abcdef"[value & 0x0F];
}

[[nodiscard]] inline char upper_hex_digit(int value) noexcept {
    return "0123456789ABCDEF"[value & 0x0F];
}

}  // namespace ruvia::detail

#pragma once

namespace ruvia {

[[nodiscard]] constexpr int decodeHexNibble(char c) noexcept {
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

[[nodiscard]] constexpr char lowerHexDigit(int value) noexcept {
    return "0123456789abcdef"[value & 0x0F];
}

[[nodiscard]] constexpr char upperHexDigit(int value) noexcept {
    return "0123456789ABCDEF"[value & 0x0F];
}

}  // namespace ruvia

#pragma once

#include <string_view>

namespace ruvia::detail {

// RFC 4648 §5 base64url alphabet (URL-safe: '-'/'_' encode 62/63, no padding in
// the canonical form). Single owner of the base64url character mapping, shared by
// JWT (RFC 7515) and the HTTP/2 upgrade HTTP2-Settings decode (RFC 7540 §3.2.1).
// Each caller keeps its own framing rules (padding, canonicality, length checks)
// around this per-character map, which is the only part they share verbatim.
inline constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Decode one base64url character to its 6-bit value, or -1 if it is not a
// base64url character ('+', '/', '=', whitespace, etc. all return -1).
[[nodiscard]] inline int decodeBase64UrlChar(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '-') {
        return 62;
    }
    if (ch == '_') {
        return 63;
    }
    return -1;
}

}  // namespace ruvia::detail

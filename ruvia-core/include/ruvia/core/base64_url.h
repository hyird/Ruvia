#pragma once

#include <string_view>

namespace ruvia {

inline constexpr std::string_view base64_url_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] inline int decode_base64_url_char(char ch) noexcept {
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

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::base64_url_alphabet;
using ::ruvia::decode_base64_url_char;
}  // namespace ruvia::detail

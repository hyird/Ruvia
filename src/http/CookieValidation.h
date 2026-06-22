#pragma once

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpTypes.h"

#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool isValidCookieValue(std::string_view value) noexcept {
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool isValidCookieAttribute(std::string_view value) noexcept {
    for (const auto c : value) {
        if (c == '\r' || c == '\n' || c == '\0' || c == ';') {
            return false;
        }
    }
    return true;
}

inline void validateCookie(std::string_view name, std::string_view value, const CookieOptions& options) {
    if (!isValidHttpHeaderName(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!isValidCookieValue(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    if (!isValidCookieAttribute(options.path) ||
        !isValidCookieAttribute(options.domain) ||
        !isValidCookieAttribute(options.sameSite)) {
        throw std::invalid_argument("invalid cookie attribute");
    }
}

}  // namespace ruvia::detail

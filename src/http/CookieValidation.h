#pragma once

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpTypes.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// RFC 6265bis: cookie lifetimes SHOULD NOT exceed 400 days.
inline constexpr std::int64_t kMaxCookieAgeSeconds = 34560000;

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

// Case-insensitive match to the canonical Priority token; empty on no match.
[[nodiscard]] inline std::string_view cookiePriorityToken(std::string_view priority) noexcept {
    constexpr std::string_view tokens[] = {"Low", "Medium", "High"};
    for (const auto token : tokens) {
        if (priority.size() != token.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t i = 0; i < token.size(); ++i) {
            const auto left = static_cast<unsigned char>(priority[i]);
            const auto right = static_cast<unsigned char>(token[i]);
            if ((left | 0x20) != (right | 0x20)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return token;
        }
    }
    return {};
}

[[nodiscard]] inline std::string_view cookiePrefixText(CookiePrefix prefix) noexcept {
    switch (prefix) {
        case CookiePrefix::kNone: return {};
        case CookiePrefix::kSecure: return "__Secure-";
        case CookiePrefix::kHost: return "__Host-";
    }
    return {};
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
    if (options.maxAge > kMaxCookieAgeSeconds) {
        throw std::invalid_argument("cookie Max-Age must not exceed 400 days");
    }
    if (options.expires.has_value() &&
        *options.expires > std::chrono::system_clock::now() + std::chrono::seconds(kMaxCookieAgeSeconds)) {
        throw std::invalid_argument("cookie Expires must not exceed 400 days ahead");
    }
    if (!options.priority.empty() && cookiePriorityToken(options.priority).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.partitioned && !options.secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    switch (options.prefix) {
        case CookiePrefix::kNone:
            break;
        case CookiePrefix::kSecure:
            if (!options.secure) {
                throw std::invalid_argument("__Secure- cookie requires Secure");
            }
            break;
        case CookiePrefix::kHost:
            if (!options.secure || options.path != "/" || !options.domain.empty()) {
                throw std::invalid_argument("__Host- cookie requires Secure, Path=/, and no Domain");
            }
            break;
    }
}

}  // namespace ruvia::detail

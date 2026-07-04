#pragma once

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
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

// Case-insensitive match of `value` against a fixed set of canonical tokens,
// returning the canonical spelling (so a caller emits a normalized attribute)
// or an empty view on no match. The Priority and SameSite validators differ
// only in their token set, so both share this scan. Every token is ASCII
// letters, for which the `| 0x20` case-fold is exact: no non-letter byte can
// collide with a letter under it, so a stray symbol never spoofs a token.
[[nodiscard]] inline std::string_view matchCanonicalToken(
    std::string_view value,
    std::span<const std::string_view> tokens) noexcept {
    for (const auto token : tokens) {
        if (value.size() != token.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t i = 0; i < token.size(); ++i) {
            const auto left = static_cast<unsigned char>(value[i]);
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

// Case-insensitive match to the canonical Priority token; empty on no match.
[[nodiscard]] inline std::string_view cookiePriorityToken(std::string_view priority) noexcept {
    static constexpr std::string_view tokens[] = {"Low", "Medium", "High"};
    return matchCanonicalToken(priority, tokens);
}

// Case-insensitive match to a canonical SameSite token; empty on no match.
[[nodiscard]] inline std::string_view cookieSameSiteToken(std::string_view sameSite) noexcept {
    static constexpr std::string_view tokens[] = {"Strict", "Lax", "None"};
    return matchCanonicalToken(sameSite, tokens);
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
    if (!options.sameSite.empty()) {
        // Only Strict/Lax/None are valid; a typo would otherwise be emitted verbatim and silently
        // treated as the browser default. RFC 6265bis §5.5: SameSite=None requires Secure.
        const auto sameSite = cookieSameSiteToken(options.sameSite);
        if (sameSite.empty()) {
            throw std::invalid_argument("invalid cookie SameSite");
        }
        if (sameSite == "None" && !options.secure) {
            throw std::invalid_argument("SameSite=None cookie requires Secure");
        }
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

#pragma once

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
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
    // A cookie attribute (Path/Domain) is emitted verbatim inside the Set-Cookie
    // field value, which setCookie writes raw -- so it must satisfy the same HTTP
    // field-value rule as every other header value (RFC 9110 5.5: no CTLs except
    // HTAB, no DEL). Rejecting only CR/LF/NUL, as before, blocked response splitting
    // but still let other control bytes (VT, FF, DEL, ...) reach the wire. ';' is a
    // valid field-value octet but delimits attributes, so it stays forbidden inside
    // one attribute to prevent a spurious attribute being injected.
    for (const auto c : value) {
        if (!isHttpFieldValueChar(static_cast<unsigned char>(c)) || c == ';') {
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

// Case-insensitive check that `name` begins with `lowerPrefix` (which must be
// given lowercase). RFC 6265bis §4.1.3 matches the __Host-/__Secure- prefixes
// case-insensitively.
[[nodiscard]] inline bool cookieNameHasPrefix(std::string_view name, std::string_view lowerPrefix) noexcept {
    if (name.size() < lowerPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lowerPrefix.size(); ++i) {
        auto c = static_cast<unsigned char>(name[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c + ('a' - 'A'));
        }
        if (static_cast<char>(c) != lowerPrefix[i]) {
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
    // RFC 6265bis §4.1.3: the __Host-/__Secure- rules apply to the cookie's actual
    // wire name -- cookiePrefixText(prefix) + name -- however the prefix was formed.
    // Keying only on the CookiePrefix enum let a hand-typed "__Host-"/"__Secure-"
    // name passed with prefix=kNone ship without the required attributes, producing
    // a cookie every browser silently drops. Derive the effective prefix from the
    // wire name (case-insensitive) so the enum and literal-name routes enforce
    // identically. When the enum sets a prefix, it is exactly the wire prefix; a
    // present enum thus takes precedence over any prefix-looking bytes in `name`.
    const bool hostPrefixed = options.prefix == CookiePrefix::kHost ||
        (options.prefix == CookiePrefix::kNone && cookieNameHasPrefix(name, "__host-"));
    const bool securePrefixed = options.prefix == CookiePrefix::kSecure ||
        (options.prefix == CookiePrefix::kNone && cookieNameHasPrefix(name, "__secure-"));
    if (hostPrefixed) {
        if (!options.secure || options.path != "/" || !options.domain.empty()) {
            throw std::invalid_argument("__Host- cookie requires Secure, Path=/, and no Domain");
        }
    } else if (securePrefixed) {
        if (!options.secure) {
            throw std::invalid_argument("__Secure- cookie requires Secure");
        }
    }
}

}  // namespace ruvia::detail

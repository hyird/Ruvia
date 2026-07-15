#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/Cookies.h"

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

[[nodiscard]] inline bool isValidCookieDomain(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    if (value.size() > 253) {
        return false;
    }

    std::size_t labelLength = 0;
    bool labelEndsWithAlnum = false;
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool alnum = (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9');
        if (c == '.') {
            if (labelLength == 0 || !labelEndsWithAlnum) {
                return false;
            }
            labelLength = 0;
            labelEndsWithAlnum = false;
            continue;
        }
        if ((!alnum && c != '-') || labelLength == 63 ||
            (labelLength == 0 && !alnum)) {
            return false;
        }
        ++labelLength;
        labelEndsWithAlnum = alnum;
    }
    return labelLength != 0 && labelEndsWithAlnum;
}

[[nodiscard]] inline std::string_view cookiePriorityToken(CookiePriority priority) noexcept {
    switch (priority) {
        case CookiePriority::kLow: return "Low";
        case CookiePriority::kMedium: return "Medium";
        case CookiePriority::kHigh: return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookieSameSiteToken(CookieSameSite sameSite) noexcept {
    switch (sameSite) {
        case CookieSameSite::kStrict: return "Strict";
        case CookieSameSite::kLax: return "Lax";
        case CookieSameSite::kNone: return "None";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookiePrefixText(CookiePrefix prefix) noexcept {
    switch (prefix) {
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
    if (!isValidCookieAttribute(options.path)) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!isValidCookieDomain(options.domain)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.maxAge.has_value()) {
        if (options.maxAge->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        if (options.maxAge->count() > kMaxCookieAgeSeconds) {
            throw std::invalid_argument("cookie Max-Age must not exceed 400 days");
        }
    }
    if (options.expires.has_value() &&
        *options.expires > std::chrono::system_clock::now() + std::chrono::seconds(kMaxCookieAgeSeconds)) {
        throw std::invalid_argument("cookie Expires must not exceed 400 days ahead");
    }
    if (options.priority && cookiePriorityToken(*options.priority).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.sameSite && cookieSameSiteToken(*options.sameSite).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.sameSite == CookieSameSite::kNone && !options.secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix && cookiePrefixText(*options.prefix).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (options.partitioned && !options.secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // RFC 6265bis §4.1.3: the __Host-/__Secure- rules apply to the cookie's actual
    // wire name -- cookiePrefixText(prefix) + name -- however the prefix was formed.
    // Keying only on the optional CookiePrefix let a hand-typed
    // "__Host-"/"__Secure-" name passed without a prefix ship without the
    // required attributes, producing
    // a cookie every browser silently drops. Derive the effective prefix from the
    // wire name (case-insensitive) so the enum and literal-name routes enforce
    // identically. When the enum sets a prefix, it is exactly the wire prefix; a
    // present enum thus takes precedence over any prefix-looking bytes in `name`.
    const bool hostPrefixed = options.prefix == CookiePrefix::kHost ||
        (!options.prefix && cookieNameHasPrefix(name, "__host-"));
    const bool securePrefixed = options.prefix == CookiePrefix::kSecure ||
        (!options.prefix && cookieNameHasPrefix(name, "__secure-"));
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

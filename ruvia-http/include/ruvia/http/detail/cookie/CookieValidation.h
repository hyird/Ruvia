#pragma once

#include "ruvia/http/HttpHeader.h"

#include "ruvia/http/detail/util/AsciiCase.h"
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
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool cookieNameStartsWithIgnoreCase(
    std::string_view name, std::string_view prefix) noexcept {
    return name.size() >= prefix.size() &&
           httpAsciiEqualsIgnoreCase(name.substr(0, prefix.size()), prefix);
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
        const bool alnum = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9');
        if (c == '.') {
            if (labelLength == 0 || !labelEndsWithAlnum) {
                return false;
            }
            labelLength = 0;
            labelEndsWithAlnum = false;
            continue;
        }
        if ((!alnum && c != '-') || labelLength == 63 || (labelLength == 0 && !alnum)) {
            return false;
        }
        ++labelLength;
        labelEndsWithAlnum = alnum;
    }
    return labelLength != 0 && labelEndsWithAlnum;
}

[[nodiscard]] inline std::string_view cookiePriorityToken(CookiePriority priority) noexcept {
    switch (priority) {
        case CookiePriority::kLow:
            return "Low";
        case CookiePriority::kMedium:
            return "Medium";
        case CookiePriority::kHigh:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookieSameSiteToken(CookieSameSite sameSite) noexcept {
    switch (sameSite) {
        case CookieSameSite::kStrict:
            return "Strict";
        case CookieSameSite::kLax:
            return "Lax";
        case CookieSameSite::kNone:
            return "None";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookiePrefixText(CookiePrefix prefix) noexcept {
    switch (prefix) {
        case CookiePrefix::kSecure:
            return "__Secure-";
        case CookiePrefix::kHost:
            return "__Host-";
    }
    return {};
}

[[nodiscard]] inline bool cookieAttributeEmitted(CookieAttributePolicy policy) {
    switch (policy) {
        case CookieAttributePolicy::kOmit:
            return false;
        case CookieAttributePolicy::kEmit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

inline void validateCookie(
    std::string_view name, std::string_view value, const CookieOptions& options) {
    const auto httpOnly = cookieAttributeEmitted(options.httpOnly);
    const auto secure = cookieAttributeEmitted(options.secure);
    const auto partitioned = cookieAttributeEmitted(options.partitioned);
    (void)httpOnly;
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
        *options.expires >
            std::chrono::system_clock::now() + std::chrono::seconds(kMaxCookieAgeSeconds)) {
        throw std::invalid_argument("cookie Expires must not exceed 400 days ahead");
    }
    if (options.priority && cookiePriorityToken(*options.priority).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.sameSite && cookieSameSiteToken(*options.sameSite).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.sameSite == CookieSameSite::kNone && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix && cookiePrefixText(*options.prefix).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const bool hostPrefixed = options.prefix == CookiePrefix::kHost ||
                              (!options.prefix && cookieNameStartsWithIgnoreCase(name, "__Host-"));
    const bool securePrefixed =
        options.prefix == CookiePrefix::kSecure ||
        (!options.prefix && cookieNameStartsWithIgnoreCase(name, "__Secure-"));
    if (hostPrefixed) {
        if (!secure || options.path != "/" || !options.domain.empty()) {
            throw std::invalid_argument("__Host- cookie requires Secure, Path=/, and no Domain");
        }
    } else if (securePrefixed) {
        if (!secure) {
            throw std::invalid_argument("__Secure- cookie requires Secure");
        }
    }
}

}  // namespace ruvia::detail

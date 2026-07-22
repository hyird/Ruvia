#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpQualityValue.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

// Media-type syntax (RFC 9110 section 8.3.1): splitting a field value into
// type/subtype and parameters, comparing parameter values across token and
// quoted-string forms, and validating a Content-Type field. Matching a media
// type against an Accept media-range lives in HttpAcceptMediaType.h.

namespace ruvia::detail {

[[nodiscard]] inline std::string_view httpMediaTypeOnly(std::string_view value) noexcept {
    return httpHeaderTokenBeforeParameters(value);
}

template <HttpTemporaryOwningCharString Value>
std::string_view httpMediaTypeOnly(Value&&) = delete;

[[nodiscard]] inline bool httpMediaToken(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    return std::ranges::all_of(token, [](char ch) noexcept {
        return isHttpTokenChar(static_cast<unsigned char>(ch));
    });
}

struct HttpMediaTypeParts final {
    std::string_view type;
    std::string_view subtype;
};

// Compare media-type parameter values after removing quoted-string syntax and
// decoding quoted-pairs.  A token and its quoted equivalent therefore compare
// equal (for example, utf-8 and "utf-8") without allocating temporary strings.
// Parameter values are otherwise case-sensitive; individual media-type
// registrations define any value-specific case folding.
[[nodiscard]] inline bool httpMediaParameterValueEquals(
    std::string_view left,
    std::string_view right,
    bool asciiCaseInsensitive = false) noexcept {
    struct Cursor final {
        std::string_view value;
        std::size_t position{0};
        std::size_t end{0};
        bool quoted{false};
        bool valid{true};

        explicit Cursor(std::string_view input) noexcept : value(httpTrimOws(input)) {
            if (value.empty()) {
                valid = false;
                return;
            }
            if (value.front() == '"') {
                if (value.size() < 2 || value.back() != '"') {
                    valid = false;
                    return;
                }
                quoted = true;
                position = 1;
                end = value.size() - 1;
                return;
            }
            end = value.size();
            for (const auto ch : value) {
                if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
                    valid = false;
                    return;
                }
            }
        }

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (!valid || position >= end) {
                return false;
            }
            auto ch = static_cast<unsigned char>(value[position++]);
            if (quoted) {
                if (ch == '\\') {
                    if (position >= end) {
                        valid = false;
                        return false;
                    }
                    ch = static_cast<unsigned char>(value[position++]);
                } else if (ch == '"' || !isHttpFieldValueChar(ch)) {
                    valid = false;
                    return false;
                }
                if (!isHttpFieldValueChar(ch)) {
                    valid = false;
                    return false;
                }
            }
            out = ch;
            return true;
        }
    };

    Cursor lhs(left);
    Cursor rhs(right);
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    while (true) {
        unsigned char lhsChar = 0;
        unsigned char rhsChar = 0;
        const bool hasLeft = lhs.next(lhsChar);
        const bool hasRight = rhs.next(rhsChar);
        if (!lhs.valid || !rhs.valid) {
            return false;
        }
        if (hasLeft != hasRight) {
            return false;
        }
        if (!hasLeft) {
            return true;
        }
        if (asciiCaseInsensitive) {
            lhsChar = httpAsciiToLower(lhsChar);
            rhsChar = httpAsciiToLower(rhsChar);
        }
        if (lhsChar != rhsChar) {
            return false;
        }
    }
}

template <typename Visitor>
[[nodiscard]] inline bool httpVisitMediaTypeParameters(
    std::string_view value,
    bool skipQualityParameter,
    Visitor&& visitor) noexcept {
    if (!httpAcceptParametersHaveStrictEquals(value)) {
        return false;
    }
    // Registered media types forbid duplicate parameter names. Keep a small,
    // fixed view table so validation stays allocation-free and bounded even for
    // hostile field values; an implausibly parameter-heavy item is invalidated.
    std::array<std::string_view, 64> names{};
    std::size_t nameCount = 0;
    auto start = httpFindUnquotedDelimiter(value, 0, ';');
    if (start >= value.size()) {
        return true;
    }
    ++start;
    while (start <= value.size()) {
        const auto end = httpFindUnquotedDelimiter(value, start, ';');
        const auto part = httpTrimOws(value.substr(start, end - start));
        const auto equals = part.find('=');
        if (part.empty() || equals == std::string_view::npos) {
            return false;
        }
        const auto name = httpTrimOws(part.substr(0, equals));
        const auto parameterValue = httpTrimOws(part.substr(equals + 1));
        if (!httpMediaToken(name)) {
            return false;
        }
        for (std::size_t index = 0; index < nameCount; ++index) {
            if (httpAsciiEqualsIgnoreCase(names[index], name)) {
                return false;
            }
        }
        if (nameCount == names.size()) {
            return false;
        }
        names[nameCount++] = name;
        if (skipQualityParameter && httpAsciiEqualsIgnoreCase(name, "q")) {
            // RFC 9110 removed the old accept-ext grammar. q is the weight
            // wherever it appears, but media-range parameters after it still
            // participate in matching, so skip q itself and continue scanning.
            if (end >= value.size()) {
                return true;
            }
            start = end + 1;
            continue;
        }
        // Comparing a value with itself performs syntax validation as well.
        if (!httpMediaParameterValueEquals(parameterValue, parameterValue) ||
            !visitor(name, parameterValue)) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline bool httpOfferedMediaTypeHasParameter(
    std::string_view offered,
    std::string_view expectedName,
    std::string_view expectedValue) noexcept {
    bool found = false;
    const bool valid = httpVisitMediaTypeParameters(
        offered,
        false,
        [expectedName, expectedValue, &found](
            std::string_view name,
            std::string_view value) noexcept {
            if (httpAsciiEqualsIgnoreCase(name, expectedName) &&
                httpMediaParameterValueEquals(
                    value,
                    expectedValue,
                    httpAsciiEqualsIgnoreCase(name, "charset"))) {
                found = true;
            }
            return true;
        });
    return valid && found;
}

[[nodiscard]] inline bool httpParseMediaTypeParts(
    std::string_view value,
    bool allowWildcard,
    HttpMediaTypeParts& parts) noexcept {
    value = httpMediaTypeOnly(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type = value.substr(0, slash);
    parts.subtype = value.substr(slash + 1);
    const bool typeWildcard = parts.type == "*";
    const bool subtypeWildcard = parts.subtype == "*";
    if ((typeWildcard || subtypeWildcard) && !allowWildcard) {
        return false;
    }
    if (typeWildcard && !subtypeWildcard) {
        return false;
    }
    return (typeWildcard || httpMediaToken(parts.type)) &&
        (subtypeWildcard || httpMediaToken(parts.subtype));
}

template <HttpTemporaryOwningCharString Value>
bool httpParseMediaTypeParts(Value&&, bool, HttpMediaTypeParts&) = delete;

[[nodiscard]] inline bool httpParseMediaType(
    std::string_view value,
    bool allowWildcard,
    HttpMediaTypeParts& parts) noexcept {
    return httpParseMediaTypeParts(value, allowWildcard, parts) &&
        httpVisitMediaTypeParameters(
            value,
            false,
            [](std::string_view, std::string_view) noexcept {
                return true;
            });
}

template <HttpTemporaryOwningCharString Value>
bool httpParseMediaType(Value&&, bool, HttpMediaTypeParts&) = delete;

[[nodiscard]] inline bool isValidHttpContentTypeFieldValue(
    std::string_view value) noexcept {
    HttpMediaTypeParts parts;
    return httpParseMediaType(value, false, parts);
}

template <HttpTemporaryOwningCharString Value>
bool isValidHttpContentTypeFieldValue(Value&&) = delete;

}  // namespace ruvia::detail

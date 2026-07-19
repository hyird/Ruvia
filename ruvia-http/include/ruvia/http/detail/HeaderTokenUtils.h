#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/BorrowedView.h"
#include "ruvia/http/detail/HttpOws.h"

namespace ruvia::detail {

// Append `value` to `out`, decoding RFC 7230 §3.2.6 quoted-pairs ("\X" -> "X").
// `value` must be a quote-trimmed parameter value: a valid unquoted token cannot
// contain a backslash, so any '\' present came from a quoted-string and is an
// escape. (A trailing lone '\' -- only possible from malformed input -- is emitted
// verbatim.) Used to unescape multipart Content-Disposition name/filename.
inline void httpAppendDecodedQuotedPairs(std::pmr::string& out, std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
        }
        out.push_back(value[i]);
    }
}

[[nodiscard]] inline std::string_view httpTrimQuotes(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

template <HttpTemporaryOwningCharString Value>
std::string_view httpTrimQuotes(Value&&) = delete;

template <typename Predicate>
[[nodiscard]] inline std::string_view httpFindHeaderToken(std::string_view value, Predicate&& predicate) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (!token.empty() && predicate(token)) {
            return token;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return {};
}

template <HttpTemporaryOwningCharString Value, typename Predicate>
std::string_view httpFindHeaderToken(Value&&, Predicate&&) = delete;

// Index of the next `delimiter` in `value` at/after `start` that is not inside an
// RFC quoted-string (honoring quoted-pairs, so a `\"` does not end the string), or
// value.size() if there is none. Sole owner of the quote-aware delimiter scan
// shared by the comma-list and semicolon-parameter quoted visitors below.
[[nodiscard]] inline std::size_t httpFindUnquotedDelimiter(
    std::string_view value, std::size_t start, char delimiter) noexcept {
    bool inQuotes = false;
    for (std::size_t i = start; i < value.size(); ++i) {
        const char c = value[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < value.size()) {
                ++i;
            } else if (c == '"') {
                inQuotes = false;
            }
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == delimiter) {
            return i;
        }
    }
    return value.size();
}

// Iterate every item in a comma-delimited HTTP list whose items may contain
// quoted-string parameters. A comma inside "..." is data, not a list separator.
// Empty items are still reported; framing-sensitive callers can reject them.
template <typename Visitor>
inline void httpVisitCommaSeparatedQuotedItems(std::string_view value, Visitor&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = httpFindUnquotedDelimiter(value, start, ',');
        const auto item = httpTrimOws(value.substr(start, end - start));
        if (!visitor(item)) {
            return;
        }
        if (end >= value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Accept-style negotiation fields can skip empty list items; stricter headers
// should use httpVisitCommaSeparatedQuotedItems directly.
template <typename Visitor>
inline void httpVisitCommaSeparatedQuoted(std::string_view value, Visitor&& visitor) {
    httpVisitCommaSeparatedQuotedItems(value, [&visitor](std::string_view item) {
        return item.empty() || visitor(item);
    });
}

// Iterate the `key=value` parameters of a `;`-delimited list (cookie pairs,
// Content-Type / Content-Disposition parameters). Each key and value is trimmed
// of OWS; segments without '=' are skipped. The visitor returns false to stop
// (e.g. once it has found the parameter it wants). Quote-stripping and key
// matching are left to the caller, which differs per RFC.
// Split one already-OWS-trimmed "name=value" segment on its first '=', trim OWS
// from each side, and hand the pair to the visitor. Sole owner of the parameter
// key/value emit shared by both semicolon scanners below. Returns the visitor's
// keep-going result; a segment with no '=' is not a parameter, so it is skipped
// (returns true to continue the scan).
template <typename Visitor>
[[nodiscard]] inline bool httpEmitSemicolonParameter(std::string_view part, Visitor&& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(httpTrimOws(part.substr(0, equals)), httpTrimOws(part.substr(equals + 1)));
}

template <typename Visitor>
inline void httpVisitSemicolonParameters(std::string_view value, Visitor&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        if (!httpEmitSemicolonParameter(part, visitor)) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

// Like httpVisitSemicolonParameters, but treats an RFC quoted-string value as
// opaque so a ';' inside a "..." value does not split the parameter. Use for
// Content-Type / Content-Disposition parameters (RFC 7231 §3.1.1.1, RFC 6266),
// whose values may be quoted-strings. Do NOT use for Cookie headers: RFC 6265
// gives '"' no special meaning there, so cookie parsing must stay literal.
template <typename Visitor>
inline void httpVisitSemicolonParametersQuoted(std::string_view value, Visitor&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = httpFindUnquotedDelimiter(value, start, ';');
        const auto part = httpTrimOws(value.substr(start, end - start));
        if (!httpEmitSemicolonParameter(part, visitor)) {
            return;
        }
        if (end >= value.size()) {
            return;
        }
        start = end + 1;
    }
}

[[nodiscard]] inline std::optional<std::string_view> httpFindSemicolonParameter(
    std::string_view value,
    std::string_view name) {
    std::optional<std::string_view> result;
    httpVisitSemicolonParameters(value, [name, &result](std::string_view key, std::string_view parameterValue) {
        if (key == name) {
            result = parameterValue;
        }
        return true;
    });
    return result;
}

template <HttpTemporaryOwningCharString Value>
std::optional<std::string_view> httpFindSemicolonParameter(
    Value&&,
    std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> httpFindSemicolonParameterQuoted(
    std::string_view value,
    std::string_view name) {
    std::optional<std::string_view> result;
    httpVisitSemicolonParametersQuoted(value, [name, &result](std::string_view key, std::string_view parameterValue) {
        if (key == name) {
            result = parameterValue;
        }
        return true;
    });
    return result;
}

template <HttpTemporaryOwningCharString Value>
std::optional<std::string_view> httpFindSemicolonParameterQuoted(
    Value&&,
    std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> httpFindSemicolonParameterQuotedIgnoreCase(
    std::string_view value,
    std::string_view name) {
    std::optional<std::string_view> result;
    httpVisitSemicolonParametersQuoted(value, [name, &result](std::string_view key, std::string_view parameterValue) {
        if (httpAsciiEqualsIgnoreCase(key, name)) {
            result = parameterValue;
        }
        return true;
    });
    return result;
}

template <HttpTemporaryOwningCharString Value>
std::optional<std::string_view> httpFindSemicolonParameterQuotedIgnoreCase(
    Value&&,
    std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> httpFindSemicolonParameterIgnoreCase(
    std::string_view value,
    std::string_view name) {
    std::optional<std::string_view> result;
    httpVisitSemicolonParameters(value, [name, &result](std::string_view key, std::string_view parameterValue) {
        if (httpAsciiEqualsIgnoreCase(key, name)) {
            result = parameterValue;
        }
        return true;
    });
    return result;
}

template <HttpTemporaryOwningCharString Value>
std::optional<std::string_view> httpFindSemicolonParameterIgnoreCase(
    Value&&,
    std::string_view) = delete;

[[nodiscard]] inline bool httpHasToken(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    const auto expectedFirst = httpAsciiToLower(static_cast<unsigned char>(expected.front()));
    return !httpFindHeaderToken(value, [expected, expectedFirst](std::string_view token) noexcept {
        if (token.size() == expected.size() &&
            httpAsciiToLower(static_cast<unsigned char>(token.front())) == expectedFirst &&
            httpAsciiEqualsIgnoreCase(token, expected)) {
            return true;
        }
        return false;
    }).empty();
}

[[nodiscard]] inline bool httpHasExactToken(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    return !httpFindHeaderToken(value, [expected](std::string_view token) noexcept {
        return token == expected;
    }).empty();
}

}  // namespace ruvia::detail

#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline unsigned char httpLowerAscii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline bool httpAsciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        auto a = static_cast<unsigned char>(left[i]);
        auto b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] inline std::string_view httpTrimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::string_view httpTrimQuotes(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

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

// Iterate the `key=value` parameters of a `;`-delimited list (cookie pairs,
// Content-Type / Content-Disposition parameters). Each key and value is trimmed
// of OWS; segments without '=' are skipped. The visitor returns false to stop
// (e.g. once it has found the parameter it wants). Quote-stripping and key
// matching are left to the caller, which differs per RFC.
template <typename Visitor>
inline void httpVisitSemicolonParameters(std::string_view value, Visitor&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        if (const auto equals = part.find('='); equals != std::string_view::npos) {
            if (!visitor(httpTrimOws(part.substr(0, equals)), httpTrimOws(part.substr(equals + 1)))) {
                return;
            }
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

[[nodiscard]] inline bool httpHasToken(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    const auto expectedFirst = httpLowerAscii(static_cast<unsigned char>(expected.front()));
    return !httpFindHeaderToken(value, [expected, expectedFirst](std::string_view token) noexcept {
        if (token.size() == expected.size() &&
            httpLowerAscii(static_cast<unsigned char>(token.front())) == expectedFirst &&
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

inline void httpUpdateConnectionFlags(
    std::string_view value,
    bool& close,
    bool& keepAlive,
    bool& upgrade) noexcept {
    while (!value.empty() && !(close && keepAlive && upgrade)) {
        const auto comma = value.find(',');
        const auto token = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (token.empty()) {
            if (comma == std::string_view::npos) {
                break;
            }
            value.remove_prefix(comma + 1);
            continue;
        }
        switch (token.size()) {
            case 5:
                if (!close &&
                    httpLowerAscii(static_cast<unsigned char>(token.front())) == 'c' &&
                    httpAsciiEqualsIgnoreCase(token, "close")) {
                    close = true;
                }
                break;
            case 7:
                if (!upgrade &&
                    httpLowerAscii(static_cast<unsigned char>(token.front())) == 'u' &&
                    httpAsciiEqualsIgnoreCase(token, "Upgrade")) {
                    upgrade = true;
                }
                break;
            case 10:
                if (!keepAlive &&
                    httpLowerAscii(static_cast<unsigned char>(token.front())) == 'k' &&
                    httpAsciiEqualsIgnoreCase(token, "keep-alive")) {
                    keepAlive = true;
                }
                break;
            default:
                break;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
}

[[nodiscard]] inline bool httpUpdateExpectContinueFlag(std::string_view value, bool& expectContinue) noexcept {
    value = httpTrimOws(value);
    if (!httpAsciiEqualsIgnoreCase(value, "100-continue")) {
        return false;
    }
    expectContinue = true;
    return true;
}

}  // namespace ruvia::detail

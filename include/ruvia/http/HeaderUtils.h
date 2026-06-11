#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia::detail {

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

template <typename Visitor>
inline void httpVisitHeaderTokens(std::string_view value, Visitor&& visitor) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (!token.empty()) {
            visitor(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
}

[[nodiscard]] inline bool httpHasToken(std::string_view value, std::string_view expected) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (!token.empty() && httpAsciiEqualsIgnoreCase(token, expected)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

inline void httpUpdateConnectionFlags(
    std::string_view value,
    bool& close,
    bool& keepAlive,
    bool& upgrade) noexcept {
    httpVisitHeaderTokens(value, [&close, &keepAlive, &upgrade](std::string_view token) noexcept {
        close = close || httpAsciiEqualsIgnoreCase(token, "close");
        keepAlive = keepAlive || httpAsciiEqualsIgnoreCase(token, "keep-alive");
        upgrade = upgrade || httpAsciiEqualsIgnoreCase(token, "Upgrade");
    });
}

[[nodiscard]] inline bool httpUpdateExpectContinueFlag(std::string_view value, bool& expectContinue) noexcept {
    value = httpTrimOws(value);
    if (!httpAsciiEqualsIgnoreCase(value, "100-continue")) {
        return false;
    }
    expectContinue = true;
    return true;
}

[[nodiscard]] inline int httpParseQualityValue(std::string_view value) noexcept {
    value = httpTrimOws(value);
    if (value == "1") {
        return 1000;
    }
    if (value == "0") {
        return 0;
    }
    if (value.size() >= 2 && value[1] == '.' && (value[0] == '0' || value[0] == '1')) {
        int quality = value[0] == '1' ? 1000 : 0;
        if (value[0] == '1') {
            for (std::size_t i = 2; i < value.size(); ++i) {
                if (value[i] != '0') {
                    return -1;
                }
            }
            return quality;
        }

        int scale = 100;
        for (std::size_t i = 2; i < value.size(); ++i) {
            if (i > 4 || value[i] < '0' || value[i] > '9') {
                return -1;
            }
            quality += (value[i] - '0') * scale;
            scale /= 10;
        }
        return quality;
    }
    return -1;
}

[[nodiscard]] inline int httpQualityParameter(std::string_view value) noexcept {
    int quality = 1000;
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        if (semicolon == std::string_view::npos) {
            break;
        }
        value.remove_prefix(semicolon + 1);
        const auto next = value.find(';');
        const auto param = httpTrimOws(next == std::string_view::npos ? value : value.substr(0, next));
        const auto equals = param.find('=');
        if (equals != std::string_view::npos) {
            const auto name = httpTrimOws(param.substr(0, equals));
            if (httpAsciiEqualsIgnoreCase(name, "q")) {
                const auto parsed = httpParseQualityValue(param.substr(equals + 1));
                return parsed < 0 ? 0 : parsed;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
    }
    return quality;
}

[[nodiscard]] inline std::string_view httpHeaderTokenBeforeParameters(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

inline void httpUpdateAcceptedEncodingQuality(
    std::string_view acceptEncoding,
    std::string_view coding,
    int& explicitQuality,
    int& wildcardQuality) noexcept;

[[nodiscard]] inline bool httpAcceptsEncoding(std::string_view acceptEncoding, std::string_view coding) noexcept {
    int explicitQuality = -1;
    int wildcardQuality = -1;
    httpUpdateAcceptedEncodingQuality(acceptEncoding, coding, explicitQuality, wildcardQuality);
    return explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality > 0;
}

inline void httpUpdateAcceptedEncodingQuality(
    std::string_view acceptEncoding,
    std::string_view coding,
    int& explicitQuality,
    int& wildcardQuality) noexcept {
    while (!acceptEncoding.empty()) {
        const auto comma = acceptEncoding.find(',');
        const auto item = httpTrimOws(
            comma == std::string_view::npos ? acceptEncoding : acceptEncoding.substr(0, comma));
        const auto token = httpHeaderTokenBeforeParameters(item);
        const auto quality = httpQualityParameter(item);
        if (httpAsciiEqualsIgnoreCase(token, coding)) {
            explicitQuality = quality;
        } else if (token == "*") {
            wildcardQuality = quality;
        }

        if (comma == std::string_view::npos) {
            break;
        }
        acceptEncoding.remove_prefix(comma + 1);
    }
}

[[nodiscard]] inline std::string_view httpMediaTypeOnly(std::string_view value) noexcept {
    return httpHeaderTokenBeforeParameters(value);
}

[[nodiscard]] inline bool httpMediaRangeMatches(std::string_view range, std::string_view offered) noexcept {
    range = httpMediaTypeOnly(range);
    offered = httpMediaTypeOnly(offered);
    const auto offeredSlash = offered.find('/');
    const auto rangeSlash = range.find('/');
    if (offeredSlash == std::string_view::npos || rangeSlash == std::string_view::npos) {
        return false;
    }

    const auto offeredType = offered.substr(0, offeredSlash);
    const auto offeredSubtype = offered.substr(offeredSlash + 1);
    const auto rangeType = range.substr(0, rangeSlash);
    const auto rangeSubtype = range.substr(rangeSlash + 1);
    if (rangeType == "*" && rangeSubtype == "*") {
        return true;
    }
    if (!httpAsciiEqualsIgnoreCase(rangeType, offeredType)) {
        return false;
    }
    return rangeSubtype == "*" || httpAsciiEqualsIgnoreCase(rangeSubtype, offeredSubtype);
}

[[nodiscard]] inline int httpMediaRangeSpecificity(std::string_view range) noexcept {
    range = httpMediaTypeOnly(range);
    const auto slash = range.find('/');
    if (slash == std::string_view::npos) {
        return -1;
    }
    const auto type = range.substr(0, slash);
    const auto subtype = range.substr(slash + 1);
    if (type == "*" && subtype == "*") {
        return 0;
    }
    if (subtype == "*") {
        return 1;
    }
    return 2;
}

[[nodiscard]] inline bool httpAcceptsMediaType(std::string_view accept, std::string_view offered) noexcept {
    if (accept.empty()) {
        return true;
    }

    int bestSpecificity = -1;
    int bestQuality = 0;
    while (!accept.empty()) {
        const auto comma = accept.find(',');
        const auto item = httpTrimOws(comma == std::string_view::npos ? accept : accept.substr(0, comma));
        if (httpMediaRangeMatches(item, offered)) {
            const auto specificity = httpMediaRangeSpecificity(item);
            const auto quality = httpQualityParameter(item);
            if (specificity > bestSpecificity || (specificity == bestSpecificity && quality > bestQuality)) {
                bestSpecificity = specificity;
                bestQuality = quality;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        accept.remove_prefix(comma + 1);
    }

    return bestSpecificity >= 0 && bestQuality > 0;
}

[[nodiscard]] inline std::optional<std::string_view> httpContentTypeParameter(
    std::string_view contentType,
    std::string_view name) noexcept {
    while (!contentType.empty()) {
        const auto semicolon = contentType.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = httpTrimOws(part.substr(0, equals));
            const auto value = httpTrimQuotes(httpTrimOws(part.substr(equals + 1)));
            if (httpAsciiEqualsIgnoreCase(key, name)) {
                return value;
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        contentType.remove_prefix(semicolon + 1);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string_view> httpHeaderValueInBlock(
    std::string_view headers,
    std::string_view name) noexcept {
    while (!headers.empty()) {
        const auto lineEnd = headers.find("\r\n");
        const auto line = lineEnd == std::string_view::npos ? headers : headers.substr(0, lineEnd);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key = httpTrimOws(line.substr(0, colon));
            if (httpAsciiEqualsIgnoreCase(key, name)) {
                return httpTrimOws(line.substr(colon + 1));
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        headers.remove_prefix(lineEnd + 2);
    }

    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string_view> httpDispositionParameter(
    std::string_view disposition,
    std::string_view name) noexcept {
    while (!disposition.empty()) {
        const auto semicolon = disposition.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? disposition : disposition.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos) {
            const auto key = httpTrimOws(part.substr(0, equals));
            const auto value = httpTrimQuotes(httpTrimOws(part.substr(equals + 1)));
            if (key == name) {
                return value;
            }
        }

        if (semicolon == std::string_view::npos) {
            break;
        }
        disposition.remove_prefix(semicolon + 1);
    }

    return std::nullopt;
}

[[nodiscard]] inline bool httpIsFormDataDisposition(std::string_view disposition) noexcept {
    const auto value = httpTrimOws(disposition);
    const auto semicolon = value.find(';');
    const auto type = httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(type, "form-data");
}

}  // namespace ruvia::detail

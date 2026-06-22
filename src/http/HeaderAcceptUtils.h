#pragma once

#include <cstddef>
#include <string_view>

#include "HeaderTokenUtils.h"

namespace ruvia::detail {

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
    int& wildcardQuality) noexcept {
    while (!acceptEncoding.empty()) {
        const auto comma = acceptEncoding.find(',');
        const auto item = httpTrimOws(
            comma == std::string_view::npos ? acceptEncoding : acceptEncoding.substr(0, comma));
        const auto token = httpHeaderTokenBeforeParameters(item);
        if (httpAsciiEqualsIgnoreCase(token, coding)) {
            explicitQuality = httpQualityParameter(item);
        } else if (token == "*") {
            wildcardQuality = httpQualityParameter(item);
        }

        if (comma == std::string_view::npos) {
            break;
        }
        acceptEncoding.remove_prefix(comma + 1);
    }
}

[[nodiscard]] inline bool httpAcceptedEncodingAllows(int explicitQuality, int wildcardQuality) noexcept {
    return explicitQuality >= 0 ? explicitQuality > 0 : wildcardQuality > 0;
}

struct HttpAcceptedEncodingQuality {
    int explicitQuality{-1};
    int wildcardQuality{-1};

    void update(std::string_view acceptEncoding, std::string_view coding) noexcept {
        httpUpdateAcceptedEncodingQuality(acceptEncoding, coding, explicitQuality, wildcardQuality);
    }

    [[nodiscard]] bool accepts() const noexcept {
        return httpAcceptedEncodingAllows(explicitQuality, wildcardQuality);
    }
};

[[nodiscard]] inline bool httpAcceptsEncoding(std::string_view acceptEncoding, std::string_view coding) noexcept {
    HttpAcceptedEncodingQuality quality;
    quality.update(acceptEncoding, coding);
    return quality.accepts();
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

}  // namespace ruvia::detail

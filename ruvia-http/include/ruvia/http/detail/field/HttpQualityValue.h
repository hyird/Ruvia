#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"

// The weight grammar every Accept-* field shares (RFC 9110 section 12.4.2): a
// qvalue parsed to thousandths, the strict `;q=` parameter syntax around it, and
// the running maximum a multi-line field folds into. Nothing here knows which
// field it is negotiating.

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

[[nodiscard]] inline bool httpAcceptParametersHaveStrictEquals(
    std::string_view value) noexcept {
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
        const auto rawName = part.substr(0, equals);
        const auto rawValue = part.substr(equals + 1);
        if (rawName.empty() || rawValue.empty() ||
            rawName != httpTrimOws(rawName) ||
            rawValue != httpTrimOws(rawValue)) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline int httpQualityParameter(std::string_view value) noexcept {
    // Reuse the shared quote-aware parameter scanner so a ';' inside a quoted media-range
    // parameter (RFC 7231 section 5.3.1: token "=" (token / quoted-string)) is not mistaken for a
    // parameter separator ; the same helper multipart Content-Type parsing uses. The leading
    // media-type / coding token has no '=', so it is skipped exactly as before; first q wins.
    if (!httpAcceptParametersHaveStrictEquals(value)) {
        return 0;
    }
    int quality = 1000;
    bool qualitySeen = false;
    bool valid = true;
    httpVisitSemicolonParametersQuoted(
        value, [&quality, &qualitySeen, &valid](
                   std::string_view name,
                   std::string_view parameter) noexcept {
            if (httpAsciiEqualsIgnoreCase(name, "q")) {
                if (qualitySeen) {
                    valid = false;
                    return false;
                }
                qualitySeen = true;
                const auto parsed = httpParseQualityValue(parameter);
                quality = parsed < 0 ? 0 : parsed;
            }
            return true;
        });
    return valid ? quality : 0;
}

[[nodiscard]] inline std::string_view httpHeaderTokenBeforeParameters(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return httpTrimOws(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

template <HttpTemporaryOwningCharString Value>
std::string_view httpHeaderTokenBeforeParameters(Value&&) = delete;

inline void httpAccumulateAcceptedQuality(int candidate, int& accumulated) noexcept {
    if (candidate > accumulated) {
        accumulated = candidate;
    }
}

}  // namespace ruvia::detail

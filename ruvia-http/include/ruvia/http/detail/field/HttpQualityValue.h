#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"

// Shared quality parsing and accumulation (RFC 9110 section 12.4.2). Token
// fields use one optional weight; media ranges extract q from their parameters.

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
                if (i > 4 || value[i] != '0') {
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

// Token-based Accept fields allow one optional weight, not media parameters.
// The list visitor has already trimmed surrounding OWS. Invalid weights are
// unacceptable rather than inheriting the unweighted default quality.
[[nodiscard]] inline int http_weight_parameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = value.substr(semicolon + 1);
    while (!weight.empty() && (weight.front() == ' ' || weight.front() == '\t')) {
        weight.remove_prefix(1);
    }
    if (weight.size() < 3 || httpAsciiToLower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != httpTrimOws(qvalue)) {
        return 0;
    }
    const auto parsed = httpParseQualityValue(qvalue);
    return parsed < 0 ? 0 : parsed;
}

[[nodiscard]] inline bool httpAcceptParametersHaveStrictEquals(std::string_view value) noexcept {
    return httpAllParameters(value, [](std::string_view part) noexcept {
        const auto equals = part.find('=');
        if (part.empty() || equals == std::string_view::npos) {
            return false;
        }
        const auto rawName = part.substr(0, equals);
        const auto rawValue = part.substr(equals + 1);
        return !rawName.empty() && !rawValue.empty() && rawName == httpTrimOws(rawName) &&
               rawValue == httpTrimOws(rawValue);
    });
}

[[nodiscard]] inline int httpQualityParameter(std::string_view value) noexcept {
    // Media-range parameters can contain quoted-string values. Reuse the shared
    // quote-aware scanner so an embedded ';' is not a parameter separator. Skip
    // the leading media type and reject duplicate weights.
    if (!httpAcceptParametersHaveStrictEquals(value)) {
        return 0;
    }
    int quality = 1000;
    bool qualitySeen = false;
    bool valid = true;
    httpVisitSemicolonParametersQuoted(
        value, [&quality, &qualitySeen, &valid](
                   std::string_view name, std::string_view parameter) noexcept {
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

[[nodiscard]] inline std::string_view httpHeaderTokenBeforeParameters(
    std::string_view value) noexcept {
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

#pragma once

#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"

namespace ruvia::detail {

// The offered media type is fully validated before matching begins.
[[nodiscard]] inline int http_media_range_matching_parameter_count(
    std::string_view range, std::string_view offered) noexcept {
    int parameter_count = 0;
    if (!httpVisitMediaTypeParameters(range, true,
            [offered, &parameter_count](std::string_view name, std::string_view value) noexcept {
                if (!httpOfferedMediaTypeHasParameter(offered, name, value)) {
                    return false;
                }
                ++parameter_count;
                return true;
            })) {
        return -1;
    }
    return parameter_count;
}

[[nodiscard]] inline int http_media_range_match_specificity(std::string_view range,
    std::string_view offered, const HttpMediaTypeParts& offered_parts) noexcept {
    HttpMediaTypeParts range_parts;
    if (!httpParseMediaTypeParts(range, true, range_parts)) {
        return -1;
    }
    if (range_parts.type != "*" && !httpAsciiEqualsIgnoreCase(range_parts.type, offered_parts.type)) {
        return -1;
    }
    if (range_parts.subtype != "*" && !httpAsciiEqualsIgnoreCase(range_parts.subtype, offered_parts.subtype)) {
        return -1;
    }
    const auto type_specificity = range_parts.type == "*" ? 0 : (range_parts.subtype == "*" ? 1 : 2);
    const auto parameter_count = http_media_range_matching_parameter_count(range, offered);
    if (parameter_count < 0) {
        return -1;
    }
    return (type_specificity << 16) | parameter_count;
}

[[nodiscard]] inline bool httpMediaRangeMatches(std::string_view range, std::string_view offered) noexcept {
    HttpMediaTypeParts offered_parts;
    return httpParseMediaType(offered, false, offered_parts) &&
           http_media_range_match_specificity(range, offered, offered_parts) >= 0;
}

// Field lines accumulate as one logical Accept value without concatenation.
inline void httpAccumulateMediaTypeAcceptance(std::string_view accept, std::string_view offered,
    int& bestSpecificity, int& bestQuality) noexcept {
    HttpMediaTypeParts offered_parts;
    if (!httpParseMediaType(offered, false, offered_parts)) {
        return;
    }
    httpVisitCommaSeparatedQuoted(accept,
        [offered, offered_parts, &bestSpecificity, &bestQuality](std::string_view item) noexcept {
            const auto specificity = http_media_range_match_specificity(item, offered, offered_parts);
            if (specificity >= 0) {
                const auto quality = httpQualityParameter(item);
                if (specificity > bestSpecificity || (specificity == bestSpecificity && quality > bestQuality)) {
                    bestSpecificity = specificity;
                    bestQuality = quality;
                }
            }
            return true;
        });
}

[[nodiscard]] inline bool httpAcceptsMediaType(std::string_view accept, std::string_view offered) noexcept {
    if (accept.empty()) {
        HttpMediaTypeParts offered_parts;
        return httpParseMediaType(offered, false, offered_parts);
    }
    int best_specificity = -1;
    int best_quality = 0;
    httpAccumulateMediaTypeAcceptance(accept, offered, best_specificity, best_quality);
    return best_specificity >= 0 && best_quality > 0;
}

}  // namespace ruvia::detail

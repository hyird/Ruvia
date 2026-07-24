#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"

// Accept negotiation (RFC 9110 section 12.5.1): whether a media-range matches an
// offered media type, how specific that match is, and the accumulator that folds
// several Accept field lines into one decision.

namespace ruvia::detail {

[[nodiscard]] inline bool httpMediaRangeMatchesValidOffered(std::string_view range, std::string_view offered, const HttpMediaTypeParts& offeredParts) noexcept {
    HttpMediaTypeParts rangeParts;
    if (!httpParseMediaTypeParts(range, true, rangeParts)) {
        return false;
    }

    if (rangeParts.type != "*" && !httpAsciiEqualsIgnoreCase(rangeParts.type, offeredParts.type)) {
        return false;
    }
    if (rangeParts.subtype != "*" && !httpAsciiEqualsIgnoreCase(rangeParts.subtype, offeredParts.subtype)) {
        return false;
    }

    // RFC 9110 section 12.5.1: media-type parameters are part of the media range
    // and must match the selected representation. q is skipped as the weight
    // wherever it appears; parameters on either side of it still participate.
    return httpVisitMediaTypeParameters(range, true, [offered](std::string_view name, std::string_view value) noexcept { return httpOfferedMediaTypeHasParameter(offered, name, value); });
}

[[nodiscard]] inline bool httpMediaRangeMatches(std::string_view range, std::string_view offered) noexcept {
    HttpMediaTypeParts offeredParts;
    return httpParseMediaType(offered, false, offeredParts) && httpMediaRangeMatchesValidOffered(range, offered, offeredParts);
}

[[nodiscard]] inline int httpMediaRangeSpecificity(std::string_view range) noexcept {
    HttpMediaTypeParts parts;
    if (!httpParseMediaTypeParts(range, true, parts)) {
        return -1;
    }
    if (parts.type == "*" && parts.subtype == "*") {
        return 0;
    }
    if (parts.subtype == "*") {
        return 1;
    }
    return 2;
}

[[nodiscard]] inline int httpMediaRangeParameterCount(std::string_view range) noexcept {
    int count = 0;
    if (!httpVisitMediaTypeParameters(range, true, [&count](std::string_view, std::string_view) noexcept {
            if (count < 0xFFFF) {
                ++count;
            }
            return true;
        })) {
        return -1;
    }
    return count;
}

// Fold the media-ranges of one Accept field value into a running best-match
// accumulator (most specific range wins; ties break on higher q). Kept separate
// from httpAcceptsMediaType so a caller with an Accept field split across several
// field lines (RFC 9110 5.3: equivalent to one comma-joined value) can feed every
// line into the SAME accumulator -- yielding the comma-joined result, including a
// q=0 exclusion whose range is more specific than an accepting range on another
// line -- without allocating to concatenate them.
inline void httpAccumulateMediaTypeAcceptance(std::string_view accept, std::string_view offered, int& bestSpecificity, int& bestQuality) noexcept {
    HttpMediaTypeParts offeredParts;
    if (!httpParseMediaType(offered, false, offeredParts)) {
        return;
    }
    httpVisitCommaSeparatedQuoted(accept, [offered, offeredParts, &bestSpecificity, &bestQuality](std::string_view item) noexcept {
        if (httpMediaRangeMatchesValidOffered(item, offered, offeredParts)) {
            const auto typeSpecificity = httpMediaRangeSpecificity(item);
            const auto parameterCount = httpMediaRangeParameterCount(item);
            if (typeSpecificity < 0 || parameterCount < 0) {
                return true;
            }
            // Type/subtype precedence dominates any number of parameters;
            // within the same range shape, more matching parameters are more
            // specific.
            const auto specificity = (typeSpecificity << 16) | parameterCount;
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
        HttpMediaTypeParts offeredParts;
        return httpParseMediaType(offered, false, offeredParts);
    }

    int bestSpecificity = -1;
    int bestQuality = 0;
    httpAccumulateMediaTypeAcceptance(accept, offered, bestSpecificity, bestQuality);
    return bestSpecificity >= 0 && bestQuality > 0;
}

}  // namespace ruvia::detail

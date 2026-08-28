#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpQualityValue.h"

// Negotiation for the Accept-* fields whose members are plain tokens rather than
// media ranges: Accept-Language, Accept-Encoding and Accept-Charset (RFC 9110
// sections 12.5.2-12.5.4). The weight grammar is shared with Accept and lives in
// HttpQualityValue.h; what differs is only how a member matches an offered
// value, which is what this header owns.

namespace ruvia::detail {

// How a member of the field matched, most specific first. Specificity decides
// before quality does, so an explicit "en-GB;q=0.5" beats a blanket "*;q=1".
enum class HttpAcceptTokenMatch : int {
    kNone = -1,
    kWildcard = 0,
    kPrefix = 1,
    kExact = 2,
};

// The member text with its parameters stripped: "en-GB;q=0.5" -> "en-GB".
[[nodiscard]] inline std::string_view httpAcceptTokenValue(std::string_view item) noexcept {
    const auto semicolon = item.find(';');
    return httpTrimOws(semicolon == std::string_view::npos ? item : item.substr(0, semicolon));
}

// RFC 4647 section 3.3.1 basic filtering: a language range matches a tag that
// equals it or extends it at a subtag boundary, so "en" matches "en-US" but
// never "english". Only Accept-Language uses this; encodings and charsets match
// exactly or by wildcard.
[[nodiscard]] inline HttpAcceptTokenMatch httpAcceptTokenMatches(std::string_view range, std::string_view offered, bool prefixMatching) noexcept {
    if (range == "*") {
        return HttpAcceptTokenMatch::kWildcard;
    }
    if (httpAsciiEqualsIgnoreCase(range, offered)) {
        return HttpAcceptTokenMatch::kExact;
    }
    if (prefixMatching && offered.size() > range.size() && offered[range.size()] == '-' && httpAsciiEqualsIgnoreCase(range, offered.substr(0, range.size()))) {
        return HttpAcceptTokenMatch::kPrefix;
    }
    return HttpAcceptTokenMatch::kNone;
}

// Folds one field line into a running best-match accumulator, for the same
// reason the media-type accumulator exists: a field split across several lines
// is equivalent to one comma-joined value (RFC 9110 5.3), including a q=0
// exclusion that is more specific than an accepting member on another line.
inline void httpAccumulateTokenAcceptance(std::string_view accept, std::string_view offered, bool prefixMatching, int& bestSpecificity, int& bestQuality) noexcept {
    httpVisitCommaSeparatedQuoted(accept, [offered, prefixMatching, &bestSpecificity, &bestQuality](std::string_view item) noexcept {
        const auto match = httpAcceptTokenMatches(httpAcceptTokenValue(item), offered, prefixMatching);
        if (match == HttpAcceptTokenMatch::kNone) {
            return true;
        }
        const auto specificity = static_cast<int>(match);
        const auto quality = httpQualityParameter(item);
        if (specificity > bestSpecificity || (specificity == bestSpecificity && quality > bestQuality)) {
            bestSpecificity = specificity;
            bestQuality = quality;
        }
        return true;
    });
}

}  // namespace ruvia::detail

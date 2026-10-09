#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

#include "field/http_quality_value.h"

// Negotiation for the Accept-* fields whose members are plain tokens rather than
// media ranges: Accept-Language and Accept-Charset (RFC 9110 sections 12.5.2 and
// 12.5.4). Accept-Encoding uses its dedicated identity and coding-alias rules.
// The weight grammar is shared with Accept and lives in
// http_quality_value.h; what differs is only how a member matches an offered
// value, which is what this header owns.

namespace ruvia::detail {

// How a member of the field matched, most specific first. Specificity decides
// before quality does, so an explicit "en-GB;q=0.5" beats a blanket "*;q=1".
enum class http_accept_token_match : int {
    none = -1,
    wildcard = 0,
    prefix = 1,
    exact = 2,
};

// The member text with its parameters stripped: "en-GB;q=0.5" -> "en-GB".
[[nodiscard]] inline std::string_view http_accept_token_value(std::string_view item) noexcept {
    const auto semicolon = item.find(';');
    return http_trim_ows(semicolon == std::string_view::npos ? item : item.substr(0, semicolon));
}

// RFC 4647 section 3.3.1 basic filtering: a language range matches a tag that
// equals it or extends it at a subtag boundary, so "en" matches "en-US" but
// never "english". Only Accept-Language uses this; charsets match exactly or by
// wildcard.
[[nodiscard]] inline http_accept_token_match http_accept_token_matches(
    std::string_view range, std::string_view offered, bool prefix_matching) noexcept {
    if (range == "*") {
        return http_accept_token_match::wildcard;
    }
    if (http_ascii_equals_ignore_case(range, offered)) {
        return http_accept_token_match::exact;
    }
    if (prefix_matching && offered.size() > range.size() && offered[range.size()] == '-' &&
        http_ascii_equals_ignore_case(range, offered.substr(0, range.size()))) {
        return http_accept_token_match::prefix;
    }
    return http_accept_token_match::none;
}

// Folds one field line into a running best-match accumulator, for the same
// reason the media-type accumulator exists: a field split across several lines
// is equivalent to one comma-joined value (RFC 9110 5.3), including a q=0
// exclusion that is more specific than an accepting member on another line.
inline void http_accumulate_token_acceptance(std::string_view accept, std::string_view offered,
    bool prefix_matching, int& best_specificity, int& best_quality) noexcept {
    http_visit_comma_separated_quoted(accept,
        [offered, prefix_matching, &best_specificity, &best_quality](std::string_view item) noexcept {
            const auto range = http_accept_token_value(item);
            const auto match = http_accept_token_matches(range, offered, prefix_matching);
            if (match == http_accept_token_match::none) {
                return true;
            }
            const auto specificity = prefix_matching && match != http_accept_token_match::wildcard
                                         ? static_cast<int>(std::min(range.size(),
                                               static_cast<std::size_t>((std::numeric_limits<int>::max)())))
                                         : static_cast<int>(match);
            const auto quality = http_weight_parameter(item);
            if (specificity > best_specificity ||
                (specificity == best_specificity && quality > best_quality)) {
                best_specificity = specificity;
                best_quality = quality;
            }
            return true;
        });
}

}  // namespace ruvia::detail

#pragma once

#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

#include "field/http_media_type.h"
#include "field/http_quality_value.h"

namespace ruvia::detail {

// The offered media type is fully validated before matching begins.
[[nodiscard]] inline int http_media_range_matching_parameter_count(
    std::string_view range, std::string_view offered) noexcept {
    int parameter_count = 0;
    if (!http_visit_media_type_parameters(range, true,
            [offered, &parameter_count](std::string_view name, std::string_view value) noexcept {
                if (!http_offered_media_type_has_parameter(offered, name, value)) {
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
    std::string_view offered, const http_media_type_parts& offered_parts) noexcept {
    http_media_type_parts range_parts;
    if (!http_parse_media_type_parts(range, true, range_parts)) {
        return -1;
    }
    if (range_parts.type_ != "*" && !http_ascii_equals_ignore_case(range_parts.type_, offered_parts.type_)) {
        return -1;
    }
    if (range_parts.subtype_ != "*" && !http_ascii_equals_ignore_case(range_parts.subtype_, offered_parts.subtype_)) {
        return -1;
    }
    const auto type_specificity = range_parts.type_ == "*" ? 0 : (range_parts.subtype_ == "*" ? 1 : 2);
    const auto parameter_count = http_media_range_matching_parameter_count(range, offered);
    if (parameter_count < 0) {
        return -1;
    }
    return (type_specificity << 16) | parameter_count;
}

[[nodiscard]] inline bool http_media_range_matches(std::string_view range, std::string_view offered) noexcept {
    http_media_type_parts offered_parts;
    return http_parse_media_type(offered, false, offered_parts) &&
           http_media_range_match_specificity(range, offered, offered_parts) >= 0;
}

// Field lines accumulate as one logical Accept value without concatenation.
inline void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered,
    int& best_specificity, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    if (!http_parse_media_type(offered, false, offered_parts)) {
        return;
    }
    http_visit_comma_separated_quoted(accept,
        [offered, offered_parts, &best_specificity, &best_quality](std::string_view item) noexcept {
            const auto specificity = http_media_range_match_specificity(item, offered, offered_parts);
            if (specificity >= 0) {
                const auto quality = http_quality_parameter(item);
                if (specificity > best_specificity || (specificity == best_specificity && quality > best_quality)) {
                    best_specificity = specificity;
                    best_quality = quality;
                }
            }
            return true;
        });
}

[[nodiscard]] inline bool http_accepts_media_type(std::string_view accept, std::string_view offered) noexcept {
    if (accept.empty()) {
        http_media_type_parts offered_parts;
        return http_parse_media_type(offered, false, offered_parts);
    }
    int best_specificity = -1;
    int best_quality = 0;
    http_accumulate_media_type_acceptance(accept, offered, best_specificity, best_quality);
    return best_specificity >= 0 && best_quality > 0;
}

}  // namespace ruvia::detail

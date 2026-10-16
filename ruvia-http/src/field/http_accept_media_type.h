#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

#include "field/http_media_type.h"
#include "field/http_quality_value.h"

namespace ruvia::detail {

[[nodiscard]] inline int http_media_range_match_specificity(std::string_view range,
    const http_media_type_parts& offered_parts, const http_media_type_parameters& offered_parameters,
    int& quality) noexcept {
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
    http_media_type_parameters parameters;
    if (!parameters.parse(range, &quality)) {
        return -1;
    }
    int parameter_count = 0;
    for (std::size_t i = 0; i < parameters.size_; ++i) {
        const auto& parameter = parameters.values_[i];
        if (http_ascii_equals_ignore_case(parameter.name(), "q")) {
            continue;
        }
        if (!offered_parameters.contains(parameter.name(), parameter.value())) {
            return -1;
        }
        ++parameter_count;
    }
    const auto type_specificity = range_parts.type_ == "*" ? 0 : (range_parts.subtype_ == "*" ? 1 : 2);
    return (type_specificity << 16) | parameter_count;
}

// Field lines accumulate as one logical Accept value without concatenation.
inline void http_accumulate_media_type_acceptance(std::string_view accept, std::string_view offered,
    int& best_specificity, int& best_quality) noexcept {
    http_media_type_parts offered_parts;
    http_media_type_parameters offered_parameters;
    if (!http_parse_media_type(offered, offered_parts, offered_parameters)) {
        return;
    }
    http_visit_comma_separated_quoted(accept,
        [offered_parts, &offered_parameters, &best_specificity, &best_quality](std::string_view item) noexcept {
            int quality = 1000;
            const auto specificity = http_media_range_match_specificity(item, offered_parts, offered_parameters, quality);
            if (specificity >= 0) {
                if (specificity > best_specificity || (specificity == best_specificity && quality > best_quality)) {
                    best_specificity = specificity;
                    best_quality = quality;
                }
            }
            return true;
        });
}

}  // namespace ruvia::detail

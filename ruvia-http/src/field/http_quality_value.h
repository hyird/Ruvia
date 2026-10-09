#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

// Shared quality parsing and accumulation (RFC 9110 section 12.4.2). Token
// fields use one optional weight; media ranges extract q from their parameters.

namespace ruvia::detail {

[[nodiscard]] inline int http_parse_quality_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
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
    if (weight.size() < 3 || http_ascii_to_lower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != http_trim_ows(qvalue)) {
        return 0;
    }
    const auto parsed_value = http_parse_quality_value(qvalue);
    return parsed_value < 0 ? 0 : parsed_value;
}

[[nodiscard]] inline bool http_accept_parameters_have_strict_equals(std::string_view value) noexcept {
    return http_all_parameters(value, [](std::string_view part) noexcept {
        const auto equals = part.find('=');
        if (part.empty() || equals == std::string_view::npos) {
            return false;
        }
        const auto raw_name = part.substr(0, equals);
        const auto raw_value = part.substr(equals + 1);
        return !raw_name.empty() && !raw_value.empty() && raw_name == http_trim_ows(raw_name) &&
               raw_value == http_trim_ows(raw_value);
    });
}

[[nodiscard]] inline int http_quality_parameter(std::string_view value) noexcept {
    // Media-range parameters can contain quoted-string values. Reuse the shared
    // quote-aware scanner so an embedded ';' is not a parameter separator. Skip
    // the leading media type and reject duplicate weights.
    if (!http_accept_parameters_have_strict_equals(value)) {
        return 0;
    }
    int quality = 1000;
    bool quality_seen = false;
    bool valid = true;
    http_visit_semicolon_parameters_quoted(
        value, [&quality, &quality_seen, &valid](
                   std::string_view name, std::string_view parameter) noexcept {
            if (http_ascii_equals_ignore_case(name, "q")) {
                if (quality_seen) {
                    valid = false;
                    return false;
                }
                quality_seen = true;
                const auto parsed_value = http_parse_quality_value(parameter);
                quality = parsed_value < 0 ? 0 : parsed_value;
            }
            return true;
        });
    return valid ? quality : 0;
}

[[nodiscard]] inline std::string_view http_header_token_before_parameters(
    std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

template <http_temporary_owning_char_string value_type>
std::string_view http_header_token_before_parameters(value_type&&) = delete;

inline void http_accumulate_accepted_quality(int candidate_value, int& accumulated) noexcept {
    if (candidate_value > accumulated) {
        accumulated = candidate_value;
    }
}

}  // namespace ruvia::detail

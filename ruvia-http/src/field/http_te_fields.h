#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_content_coding.h"

#include "coding/http_transfer_encoding.h"
#include "field/http_quality_value.h"

namespace ruvia::detail {

enum class http_te_field_validation_mode : std::uint8_t { recipient,
    client_capability };

[[nodiscard]] inline bool http_is_client_supported_te_transfer_coding(std::string_view coding) noexcept {
    return http_is_gzip_coding_token(coding) || http_ascii_equals_ignore_case(coding, "deflate");
}

[[nodiscard]] inline bool http_te_coding_allows_only_quality_parameter(std::string_view coding) noexcept {
    return http_ascii_equals_ignore_case(coding, "compress") || http_is_gzip_coding_token(coding) ||
           http_ascii_equals_ignore_case(coding, "deflate");
}

[[nodiscard]] inline bool http_te_parameters_are_valid(
    std::string_view item, http_te_field_validation_mode mode, bool only_quality_parameter) noexcept {
    auto start = http_find_unquoted_delimiter(item, 0, ';');
    if (start >= item.size()) {
        return true;
    }

    bool quality_seen = false;
    ++start;
    while (start <= item.size()) {
        const auto end = http_find_unquoted_delimiter(item, start, ';');
        const auto parameter = http_trim_ows(item.substr(start, end - start));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }

        const auto raw_name = parameter.substr(0, equals);
        const auto raw_value = parameter.substr(equals + 1);
        const auto name = http_trim_ows(raw_name);
        const auto value = http_trim_ows(raw_value);
        if (http_ascii_equals_ignore_case(name, "q")) {
            if (quality_seen || raw_name != name || raw_value != value ||
                http_parse_quality_value(value) < 0) {
                return false;
            }
            quality_seen = true;
        } else if (mode == http_te_field_validation_mode::client_capability || only_quality_parameter) {
            return false;
        }

        if (end >= item.size()) {
            return true;
        }
        start = end + 1;
    }

    return true;
}

[[nodiscard]] inline bool is_valid_http_te_field_item(
    std::string_view item, http_te_field_validation_mode mode) noexcept {
    std::string_view coding;
    bool has_parameters = false;
    if (!http_parse_transfer_coding_syntax(item, coding, has_parameters)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(coding, "trailers")) {
        return !has_parameters;
    }
    if (http_ascii_equals_ignore_case(coding, "chunked")) {
        return false;
    }
    if (mode == http_te_field_validation_mode::client_capability &&
        !http_is_client_supported_te_transfer_coding(coding)) {
        return false;
    }
    return http_te_parameters_are_valid(item, mode, http_te_coding_allows_only_quality_parameter(coding));
}

[[nodiscard]] inline bool is_valid_http_te_field_value(
    std::string_view value, http_te_field_validation_mode mode) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (http_trim_ows(value).empty()) {
        return true;
    }

    bool valid = true;
    bool saw_item = false;
    http_visit_comma_separated_quoted_items(
        value, [&valid, &saw_item, mode](std::string_view item) noexcept {
            saw_item = true;
            if (!is_valid_http_te_field_item(item, mode)) {
                valid = false;
                return false;
            }
            return true;
        });
    return valid && saw_item;
}

[[nodiscard]] inline bool is_valid_received_http_te_field_value(std::string_view value) noexcept {
    return is_valid_http_te_field_value(value, http_te_field_validation_mode::recipient);
}

[[nodiscard]] inline bool is_valid_client_http_te_field_value(std::string_view value) noexcept {
    return is_valid_http_te_field_value(value, http_te_field_validation_mode::client_capability);
}

}  // namespace ruvia::detail

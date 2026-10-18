#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_ascii.h"
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
        const auto end = start + http_find_unquoted_delimiter(item.substr(start), 0, ';');
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
    // RFC 9112 section 7.4: a client MUST NOT list chunked in TE. That binds
    // the sender; a recipient treats the member as the always-acceptable
    // chunked coding and does not reject the request.
    if (http_ascii_equals_ignore_case(coding, "chunked")) {
        return mode == http_te_field_validation_mode::recipient &&
               http_te_parameters_are_valid(item, mode, true);
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
    http_visit_comma_separated_quoted_field_items(
        value, [&valid, mode](std::string_view item) noexcept {
            // RFC 9110 section 5.6.1 requires recipients to ignore empty list members.
            if (item.empty() && mode == http_te_field_validation_mode::recipient) {
                return true;
            }
            if (!is_valid_http_te_field_item(item, mode)) {
                valid = false;
                return false;
            }
            return true;
        });
    return valid;
}

[[nodiscard]] inline bool is_valid_received_http_te_field_value(std::string_view value) noexcept {
    return is_valid_http_te_field_value(value, http_te_field_validation_mode::recipient);
}

[[nodiscard]] inline bool is_valid_client_http_te_field_value(std::string_view value) noexcept {
    return is_valid_http_te_field_value(value, http_te_field_validation_mode::client_capability);
}

// HTTP/2 carries no transfer codings, so TE may only advertise
// "trailers" (RFC 9113 section 8.2.2). The member is an
// unprefixed ABNF literal and therefore case-insensitive (RFC 7405 section
// 2.1); list members are OWS-trimmed (RFC 9110 section 5.6.1). Recipients
// ignore empty members and accept empty lists (RFC 9110 section 10.1.4).
// This sender API requires a nonempty list without empty members.
[[nodiscard]] inline bool is_valid_binary_http_te_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    bool valid = true;
    bool has_member = false;
    http_visit_comma_separated_quoted_field_items(value, [&valid, &has_member, role](std::string_view member) noexcept {
        if (member.empty() ? role == http_field_list_role::sender
                           : !http_ascii_equals_ignore_case(member, "trailers")) {
            valid = false;
            return false;
        }
        has_member = has_member || !member.empty();
        return true;
    });
    return valid && (role == http_field_list_role::recipient || has_member);
}

}  // namespace ruvia::detail

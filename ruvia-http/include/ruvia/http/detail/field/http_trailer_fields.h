#pragma once

#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"

namespace ruvia::detail {

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] inline bool is_forbidden_common_trailer_name(
    std::string_view name, request_header_kind kind) noexcept {
    switch (kind) {
        case request_header_kind::host:
        case request_header_kind::content_length:
        case request_header_kind::transfer_encoding:
        case request_header_kind::connection:
        case request_header_kind::content_encoding:
        case request_header_kind::content_type:
        case request_header_kind::cookie:
        case request_header_kind::expect:
        case request_header_kind::if_match:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_none_match:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::range:
        case request_header_kind::upgrade:
        case request_header_kind::authorization:
            return true;
        case request_header_kind::other:
        case request_header_kind::accept:
        case request_header_kind::accept_encoding:
        case request_header_kind::access_control_request_headers:
        case request_header_kind::access_control_request_method:
        case request_header_kind::origin:
        case request_header_kind::user_agent:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 2:
            return http_ascii_equals_ignore_case(name, "TE");
        case 7:
            return http_ascii_equals_ignore_case(name, "Trailer");
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") ||
                   http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") ||
                   http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        case 19:
            return http_ascii_equals_ignore_case(name, "Proxy-Authorization");
        default:
            return false;
    }
}

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] inline bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_request_header(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == request_header_kind::access_control_request_headers ||
           kind == request_header_kind::access_control_request_method ||
           kind == request_header_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    const bool empty_field = http_trim_ows(value).empty();
    bool valid = true;
    http_visit_comma_separated_quoted_items(
        value, [&valid, empty_field, role, &forbidden_name](std::string_view item) noexcept {
            if (item.empty()) {
                // RFC 9110 section 5.6.1.1: senders must not generate empty list
                // elements, but `#field-name` itself can represent an empty list.
                if (role == http_field_list_role::sender && !empty_field) {
                    valid = false;
                    return false;
                }
                return true;
            }
            if (!is_valid_http_header_name(item) || forbidden_name(item)) {
                valid = false;
                return false;
            }
            return true;
        });
    return valid;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

}  // namespace ruvia::detail

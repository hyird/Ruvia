#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"

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
        case request_header_kind::te:
        case request_header_kind::trailer:
        case request_header_kind::proxy_authorization:
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
    http_visit_comma_separated_quoted_field_items(
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

enum class http_trailer_block_status : std::uint8_t {
    ok,
    invalid_field,
    forbidden_field,
    section_too_large,
    visitor_stopped,
};

// The single HTTP/1 trailer-block field parser for both message directions.
// `block` holds the field lines between the last chunk and the empty line that
// ends the trailer section; a final CRLF after the last field is accepted for
// standalone validation. Values are exposed with field-line OWS trimmed.
// `forbidden_name` is the direction's trailer policy. `budget` and
// `field_count` are the section totals before this block, so a caller can
// extend a retained section; `budget` is advanced for every accepted field.
// The visitor receives views borrowing `block` and returns false to stop.
template <typename forbidden_name_type, typename visitor_type>
[[nodiscard]] http_trailer_block_status visit_http1_trailer_block(std::string_view block,
    forbidden_name_type&& forbidden_name, http_field_section_budget& budget, std::size_t field_count,
    visitor_type&& visitor) {
    if (block.size() > max_http_header_bytes) {
        return http_trailer_block_status::section_too_large;
    }
    std::size_t cursor_value = 0;
    while (cursor_value < block.size()) {
        if (field_count == max_http_header_fields) {
            return http_trailer_block_status::section_too_large;
        }
        const auto line_end = block.find("\r\n", cursor_value);
        const auto line = line_end == std::string_view::npos
                              ? block.substr(cursor_value)
                              : block.substr(cursor_value, line_end - cursor_value);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return http_trailer_block_status::invalid_field;
        }
        const auto name = line.substr(0, colon);
        const auto value = http_trim_ows(line.substr(colon + 1));
        if (!is_valid_http_field_value_bytes(value)) {
            return http_trailer_block_status::invalid_field;
        }
        if (forbidden_name(name)) {
            return http_trailer_block_status::forbidden_field;
        }
        if (!budget.add_field(name, value)) {
            return http_trailer_block_status::section_too_large;
        }
        ++field_count;
        if (!visitor(name, value)) {
            return http_trailer_block_status::visitor_stopped;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor_value = line_end + 2;
    }
    return http_trailer_block_status::ok;
}

}  // namespace ruvia::detail

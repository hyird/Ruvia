#include "field/response_field_validation.h"

#include <stdexcept>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_response_server.h"

#include "coding/http_content_coding.h"
#include "field/http_connection_fields.h"
#include "field/http_media_type.h"
#include "parser/http_parser_syntax.h"
#include "response/http_response_header_bits.h"

namespace ruvia::detail {

bool is_forbidden_common_trailer_name(
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

bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_request_header(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == request_header_kind::access_control_request_headers ||
           kind == request_header_kind::access_control_request_method ||
           kind == request_header_kind::origin ||
           http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name);
        known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_request_header(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return http_ascii_equals_ignore_case(name, "Age");
        case 6:
            return http_ascii_equals_ignore_case(name, "Pragma");
        case 7:
            return http_ascii_equals_ignore_case(name, "Expires") ||
                   http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") ||
                   http_ascii_equals_ignore_case(name, "Referrer-Policy") ||
                   http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") ||
                   http_ascii_equals_ignore_case(name, "WWW-Authenticate");
        case 18:
            return http_ascii_equals_ignore_case(name, "Permissions-Policy");
        case 19:
            return http_ascii_equals_ignore_case(name, "Content-Disposition");
        case 22:
            return http_ascii_equals_ignore_case(name, "X-Content-Type-Options");
        case 23:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy");
        case 25:
            return http_ascii_equals_ignore_case(name, "Strict-Transport-Security");
        case 35:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

[[nodiscard]] bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

[[nodiscard]] bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

response_field_status validate_response_field(request_header_kind kind, std::string_view value,
    response_field_state& state) noexcept {
    switch (kind) {
        case request_header_kind::content_type:
            if (!is_valid_http_content_type_field_value(value)) {
                return response_field_status::invalid;
            }
            if (state.content_type_seen_) {
                return response_field_status::repeated_singleton;
            }
            state.content_type_seen_ = true;
            break;
        case request_header_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        case request_header_kind::trailer:
            if (!is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    detail::response_field_state state;
    return detail::validate_response_field(detail::classify_request_header(name), value, state) ==
           detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia

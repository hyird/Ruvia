#pragma once

#include <array>
#include <concepts>
#include <exception>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1, RFC 9113 §8.1).
[[nodiscard]] inline bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
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

[[nodiscard]] inline bool is_valid_http_response_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_response_trailer_name(name); });
}

// True if (name, value) is an acceptable HTTP/1.1 chunked response trailer.
[[nodiscard]] inline bool response_trailer_field_valid(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) &&
           is_valid_http_header_value(value);
}

}  // namespace ruvia::detail

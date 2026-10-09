#pragma once

#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/ascii_case.h"

#include "field/binary_field_name.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2_is_valid_regular_header(
    std::string_view name, std::string_view value) noexcept {
    if (!is_valid_binary_field_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    if (is_forbidden_http_binary_connection_field(name)) {
        return false;
    }
    // `trailers` is an unprefixed ABNF literal in RFC 9110 Section 10.1.4,
    // hence it is case-insensitive (RFC 7405 Section 2.1). HTTP/2 restricts TE
    // to that sole value, but does not make the field-value keyword lowercase.
    return name != "te" || http_ascii_equals_ignore_case(value, "trailers");
}

// Decoded HTTP/2 field names are already required to be lowercase. The TE
// exception above belongs only to requests (RFC 9113 Section 8.2.2); a response
// carrying TE is malformed even when its value is exactly "trailers".
[[nodiscard]] inline bool http2_is_valid_decoded_response_header(
    std::string_view name, std::string_view value) noexcept {
    return http2_is_valid_regular_header(name, value) && !is_forbidden_http_binary_response_field(name);
}

}  // namespace ruvia::detail

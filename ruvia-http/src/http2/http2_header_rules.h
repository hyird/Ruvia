#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"

#include "field/http_connection_fields.h"
#include "http2/http2_field_name.h"
#include "parser/http_parser_syntax.h"

namespace ruvia::detail {

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. Huffman coding can spend up to 30 bits per decoded
// octet, so four encoded bytes per decoded-budget byte bound a valid block while
// keeping a hard memory limit for incomplete CONTINUATION sequences.
inline constexpr std::size_t max_http2_encoded_header_block_bytes = 4 * max_http_header_bytes;

// RFC 7541 §7.1.3: credential-bearing fields use the never-indexed literal so an
// intermediary never places them in a shared dynamic table.
[[nodiscard]] inline bool hpack_header_name_is_sensitive(std::string_view name) noexcept {
    return http_ascii_equals_ignore_case(name, "authorization") ||
           http_ascii_equals_ignore_case(name, "proxy-authorization") ||
           http_ascii_equals_ignore_case(name, "cookie") ||
           http_ascii_equals_ignore_case(name, "set-cookie");
}

// Request TE values are validated with the other request-field semantics
// (validate_request_field: only "trailers", RFC 9113 Section 8.2.2).
[[nodiscard]] inline bool http2_is_valid_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_binary_field_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

[[nodiscard]] inline bool http2_is_valid_outbound_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

// Decoded HTTP/2 field names are already required to be lowercase. TE is
// permitted only in requests (RFC 9113 Section 8.2.2); a response carrying TE
// is malformed even when its value is exactly "trailers".
[[nodiscard]] inline bool http2_is_valid_decoded_response_header(
    std::string_view name, std::string_view value) noexcept {
    return http2_is_valid_regular_header(name, value) && name != "te";
}
}  // namespace ruvia::detail

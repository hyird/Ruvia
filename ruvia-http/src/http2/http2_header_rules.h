#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"

#include "field/http_connection_fields.h"
#include "field/http_request_field_validation.h"
#include "parser/http_parser_syntax.h"
#include "response/http_framed_response_head_plan.h"

namespace ruvia::detail {

// HTTP/2 header-list size (RFC 9113 section 6.5.2): each field, pseudo-fields
// included, costs its name and value plus 32 bytes, charged against
// max_http_header_bytes. This deliberately differs from the HTTP/1 field-line
// accounting of http_field_section_budget. A failed add leaves the size unchanged.
class http2_header_list_size final {
public:
    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::size_t field_metadata_bytes = 32;
        const auto remaining = max_http_header_bytes - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::size_t bytes_{0};
};

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. Huffman coding can spend up to 30 bits per decoded
// octet, so four encoded bytes per decoded-budget byte bound a valid block while
// keeping a hard memory limit for incomplete CONTINUATION sequences.
inline constexpr std::size_t max_http2_encoded_header_block_bytes = 4 * max_http_header_bytes;

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

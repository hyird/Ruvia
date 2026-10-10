#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_parse_error.h"

#include "coding/http_content_length.h"
#include "coding/http_transfer_encoding.h"

namespace ruvia::detail {

// Slice and parsed_request_header_slot carry no default member initializers on
// purpose: the fixed header table stays uninitialized per request (slots are
// always written before they are read up to header_count), so constructing a
// parsed_request_header_block does not re-zero ~1.5KB on every request.
struct http_header_slice {
    std::uint32_t offset_;
    std::uint32_t length_;

    [[nodiscard]] std::string_view bind(std::string_view buffer) const noexcept {
        return buffer.substr(offset_, length_);
    }

    template <http_temporary_owning_char_string buffer_type>
    std::string_view bind(buffer_type&&) const = delete;
};

struct parsed_request_header_slot {
    http_header_slice name_;
    http_header_slice value_;
    request_header_kind kind_;
};

using known_request_header_index_type = std::int16_t;

struct parsed_request_header_block {
    explicit parsed_request_header_block(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : transfer_encoding_(resource) {}

    http_header_slice method_;
    http_header_slice target_;
    http_header_slice version_;
    std::array<parsed_request_header_slot, max_http_header_fields> headers_;
    std::size_t header_count_{0};
    known_request_header_index_type host_header_index_{-1};
    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
    http_content_length_state<> content_length_;
    std::uint32_t seen_header_bits_{0};
    http_response_coding_qualities response_coding_qualities_;
    http_transfer_encoding_state transfer_encoding_;
    http_request_expectations expectations_;
    bool non_empty_trailer_header_present_{false};
    bool te_header_present_{false};
};

[[nodiscard]] std::size_t find_http_header_end(
    std::string_view buffer, std::size_t search_offset) noexcept;
// RFC 9112 section 2.2: a server SHOULD ignore at least one empty line received
// before the request-line. Leading CRLF or bare LF lines are skipped; they count
// toward max_http_header_bytes, which bounds how many are ignored.
[[nodiscard]] std::size_t http_request_leading_empty_line_bytes(std::string_view buffer) noexcept;
// find_http_header_end for a request head: the returned end is measured from
// the start of buffer and includes any ignored leading empty lines.
[[nodiscard]] std::size_t find_http_request_head_end(
    std::string_view buffer, std::size_t search_offset) noexcept;
// header_bytes is the find_http_request_head_end result; the request-line is
// parsed after the ignored leading empty lines.
[[nodiscard]] std::optional<http_parse_error> parse_http_header_block(
    std::string_view buffer, std::size_t header_bytes, parsed_request_header_block& block);

}  // namespace ruvia::detail

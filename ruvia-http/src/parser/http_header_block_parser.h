#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"

#include "coding/http_content_length.h"
#include "coding/http_transfer_encoding.h"
#include "field/http_request_field_validation.h"
#include "parser/http_header_descriptor_storage.h"

namespace ruvia::detail {

// Request slots are assigned only when appended; unused inline slots need no
// per-request zeroing. Offsets keep their borrowed bytes independent of moves.
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
    explicit parsed_request_header_block(std::pmr::memory_resource* resource = nullptr) noexcept
        : headers_(resource) {}

    http_header_slice method_;
    http_header_slice target_;
    http_protocol_version protocol_version_;
    http_header_descriptor_storage<parsed_request_header_slot> headers_;
    known_request_header_index_type host_header_index_{-1};
    http_connection_options connection_options_;
    http_content_length_state content_length_;
    request_field_state fields_{request_field_framing::http1};
    http_response_coding_qualities response_coding_qualities_;
    http_transfer_encoding_state transfer_encoding_;
    http_request_expectations expectations_;
};

[[nodiscard]] std::size_t find_http_header_end(
    std::string_view buffer, std::size_t search_offset) noexcept;
// True if an LF at or after search_offset (and within max_http_header_bytes)
// is not preceded by CR. Within a field section such a line can never end the
// CRLF-delimited head; the caller passes the end of the accepted request-line
// or a later offset whose earlier bytes were already scanned.
[[nodiscard]] bool has_http_bare_line_feed(std::string_view buffer, std::size_t search_offset) noexcept;
// RFC 9112 section 2.2: a server SHOULD ignore at least one empty line received
// before the request-line. Leading CRLF or bare LF lines are skipped; they count
// toward max_http_header_bytes, which bounds how many are ignored. The scan
// resumes after `skipped` bytes already classified by an earlier call.
[[nodiscard]] std::size_t http_request_leading_empty_line_bytes(
    std::string_view buffer, std::size_t skipped) noexcept;
// find_http_header_end for a request head: the returned end is measured from
// the start of buffer and includes the ignored leading empty lines, whose
// count `leading_bytes` carries in (from an earlier call) and out.
[[nodiscard]] std::size_t find_http_request_head_end(
    std::string_view buffer, std::size_t& leading_bytes, std::size_t search_offset) noexcept;
// Classifies the request-line of a still incomplete head: a request-line error
// once the first line after the leading empty lines is CRLF-terminated, or as
// soon as its first byte cannot start a method token. Bytes before
// search_offset were classified by an earlier call. An accepted line sets
// request_line_end to the offset just past its CRLF; nullopt without it means
// more input is needed.
[[nodiscard]] std::optional<http_parse_error> classify_http_request_line_prefix(std::string_view buffer,
    std::size_t leading_bytes, std::size_t search_offset, std::size_t& request_line_end) noexcept;
// header_bytes is the find_http_request_head_end result; the request-line is
// parsed after the leading_bytes ignored leading empty lines.
[[nodiscard]] std::optional<http_parse_error> parse_http_header_block(std::string_view buffer,
    std::size_t leading_bytes, std::size_t header_bytes, parsed_request_header_block& block);

}  // namespace ruvia::detail

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"

#include "coding/http_content_coding.h"
#include "coding/http_transfer_encoding.h"
#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"
#include "parser/http_header_descriptor_storage.h"

namespace ruvia::detail {

// The first input byte occupies the low byte, independent of native byte order.
// The caller must provide at least eight readable bytes.
[[nodiscard]] inline std::uint64_t load_http_field_value_word(const char* data) noexcept {
    std::uint64_t bytes;
    std::memcpy(&bytes, data, sizeof(bytes));
    if constexpr (std::endian::native == std::endian::big) {
        bytes = 0;
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            bytes |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[index])) << (index * 8);
        }
    }
    return bytes;
}

// The first set high bit identifies the first invalid input byte. DEL detection
// can also mark later bytes, but never changes the position of its first match.
[[nodiscard]] inline std::uint64_t invalid_http_field_value_mask(std::uint64_t bytes_value) noexcept {
    constexpr std::uint64_t ones = 0x0101010101010101ULL;
    constexpr std::uint64_t high_bits = ones * 0x80U;
    constexpr std::uint64_t low_bits = ones * 0x7fU;
    const auto del = bytes_value ^ low_bits;
    // Setting each high bit prevents inter-byte borrow when testing < SP.
    // Exclude original high-bit bytes so obs-text remains valid.
    const auto controls = ~((bytes_value | high_bits) - ones * 0x20U) & ~bytes_value;
    const auto special = (controls | ((del - ones) & ~del)) & high_bits;
    if (special == 0) {
        return 0;
    }
    // This exact HTAB mask cannot carry between bytes or hide a DEL match.
    const auto tabs = bytes_value ^ (ones * 9U);
    const auto tab_bits = ~(((tabs & low_bits) + low_bits) | tabs | low_bits);
    return special & ~tab_bits;
}

[[nodiscard]] inline std::size_t http_field_value_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= sizeof(std::uint64_t)) {
        const auto bytes_value = load_http_field_value_word(value.data() + index);
        if (const auto invalid = invalid_http_field_value_mask(bytes_value); invalid != 0) {
            return index + static_cast<std::size_t>(std::countr_zero(invalid)) / 8;
        }
        index += sizeof(bytes_value);
    }
    while (index < value.size() && is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class field_line_folding_policy { mime,
    http };

// A fold always starts with CRLF followed by SP/HTAB. Leave bare CR/LF and
// ordinary field separators visible to the protocol's field validator.
[[nodiscard]] inline std::size_t find_field_line_fold(
    std::string_view input, std::size_t start = 0) noexcept {
    auto position = input.find("\r\n", start);
    while (position != std::string_view::npos) {
        if (position + 2 < input.size() && is_http_ows_char(input[position + 2])) {
            return position;
        }
        position = input.find("\r\n", position + 2);
    }
    return std::string_view::npos;
}

// first_fold comes from find_field_line_fold. The caller owns storage through
// every use of the resulting views; input must not borrow that storage.
// MIME removes only CRLF. HTTP replaces the complete obs-fold, including its
// adjacent OWS/RWS, with SP before field values are interpreted (RFC 9112 5.2).
inline void copy_unfolded_field_lines(std::string_view input, std::size_t first_fold,
    field_line_folding_policy policy, std::string& storage) {
    storage.clear();
    storage.reserve(input.size());
    std::size_t segment_begin = 0;
    for (auto position = first_fold; position != std::string_view::npos;) {
        auto segment_end = position;
        auto next_segment = position + 2;
        if (policy == field_line_folding_policy::http) {
            while (segment_end > segment_begin && is_http_ows_char(input[segment_end - 1])) {
                --segment_end;
            }
            while (next_segment < input.size() && is_http_ows_char(input[next_segment])) {
                ++next_segment;
            }
        }
        storage.append(input.substr(segment_begin, segment_end - segment_begin));
        if (policy == field_line_folding_policy::http) {
            storage.push_back(' ');
        }
        segment_begin = next_segment;
        position = find_field_line_fold(input, next_segment);
    }
    storage.append(input.substr(segment_begin));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Scan one complete RFC 9112 section 5 field line, including its CRLF.
// The caller excludes the final empty line and unfolds response obs-fold first.
// Views borrow buffer; only a successful scan advances cursor_value.
[[nodiscard]] inline std::optional<http_header_view> scan_http1_field_line(
    std::string_view buffer, std::size_t& cursor_value) noexcept {
    const auto name_start = cursor_value;
    auto position = cursor_value + http_token_prefix_size(buffer.substr(cursor_value));
    if (position == name_start || position >= buffer.size() || buffer[position] != ':') {
        return std::nullopt;
    }
    const auto name = buffer.substr(name_start, position - name_start);
    ++position;
    while (position < buffer.size() && is_http_ows_char(buffer[position])) {
        ++position;
    }
    const auto value_start = position;
    position += http_field_value_prefix_size(buffer.substr(position));
    if (position + 1 >= buffer.size() || buffer[position] != '\r' || buffer[position + 1] != '\n') {
        return std::nullopt;
    }
    // Leading OWS has already been skipped; trim only the trailing OWS.
    auto value_end = position;
    while (value_end > value_start && is_http_ows_char(buffer[value_end - 1])) {
        --value_end;
    }
    cursor_value = position + 2;
    return http_header_view{name, buffer.substr(value_start, value_end - value_start)};
}

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
    http_field_kind kind_;
};

using known_request_header_index_type = std::int16_t;
// parse_http_header_block stops at max_http_header_fields, so every field index
// fits the index type and the request's descriptor block.
static_assert(max_http_header_fields <=
              static_cast<std::size_t>((std::numeric_limits<known_request_header_index_type>::max)()));

struct parsed_request_header_block {
    parsed_request_header_block() noexcept = default;

    http_header_slice method_;
    http_header_slice target_;
    http_protocol_version protocol_version_;
    http_header_descriptor_storage<parsed_request_header_slot> headers_;
    known_request_header_index_type host_header_index_{-1};
    http_connection_options connection_options_;
    http_content_length_state content_length_;
    request_field_state fields_{request_field_framing::http1};
    http_transfer_encoding_state transfer_encoding_;
    http_request_expectations expectations_;
};

[[nodiscard]] std::size_t find_http_header_end(
    std::string_view buffer, std::size_t search_offset) noexcept;
// True if an LF at or after search_offset (and within max_http_header_bytes)
// is not preceded by CR. Within a field section such a line can never end the
// CRLF-delimited head; the caller passes the end of the accepted request-line
// or status-line, or a later offset whose earlier bytes were already scanned.
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

namespace ruvia::detail {

enum class http1_version_parse_result : std::uint8_t { invalid,
    unsupported,
    http10,
    http11 };

// HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3). A higher
// HTTP/1 minor version is processed as HTTP/1.1 (RFC 9110 section 6.2).
[[nodiscard]] inline http1_version_parse_result parse_http1_version(std::string_view version) noexcept {
    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9') {
        return http1_version_parse_result::invalid;
    }
    if (version[5] != '1') {
        return http1_version_parse_result::unsupported;
    }
    return version[7] == '0' ? http1_version_parse_result::http10 : http1_version_parse_result::http11;
}

// Callers decide whether a framing field applies before accumulating it and
// retain their own error enum. Unsupported transfer codings are recorded by
// the accumulator for the caller's whole-section policy, not rejected here.
template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_content_length_field(
    std::string_view value, http_content_length_state& state) noexcept {
    switch (state.parse_field(value)) {
        case http_content_length_parse_status::ok:
            return std::nullopt;
        case http_content_length_parse_status::invalid:
            return error_type::invalid_content_length;
        case http_content_length_parse_status::conflicting:
            return error_type::conflicting_content_length;
    }
    return std::nullopt;
}

template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_transfer_encoding_field(
    std::string_view value, http_transfer_encoding_state& state) {
    if (state.parse_field(value) == http_transfer_encoding_parse_status::malformed) {
        return error_type::invalid_transfer_encoding;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

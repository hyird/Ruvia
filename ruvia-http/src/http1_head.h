#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "http_field_text.h"
#include "http_header.h"
#include "http_message_framing_fields.h"
#include "http_protocol.h"

// HTTP/1 head wire scanning shared by the server request parser and the client
// response parser: field-line scanning, head boundaries, version and framing
// field accumulation. Binary (HTTP/2, HTTP/3) paths never include this header.
namespace ruvia::detail {

// Scan one complete RFC 9112 section 5 field line, including its CRLF.
// The caller excludes the final empty line and unfolds response obs-fold first.
// Views borrow buffer; only a successful scan advances cursor_value.
[[nodiscard]] inline std::optional<http_header_view> scan_http1_field_line(std::string_view buffer, std::size_t& cursor_value) noexcept {
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

[[nodiscard]] inline std::size_t find_http_header_end(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    if (buffer.size() < 4) {
        return std::string_view::npos;
    }

    auto cursor_value = std::max<std::size_t>(3, search_offset);
    while (cursor_value < buffer.size()) {
        const auto i = buffer[cursor_value] == '\n' ? cursor_value : buffer.find('\n', cursor_value + 1);
        if (i == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (buffer[i - 1] == '\r') {
            if (buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
                return i + 1;
            }
            if (i + 2 < buffer.size() && buffer[i + 1] == '\r' && buffer[i + 2] == '\n') {
                return i + 3;
            }
            cursor_value = i + 3;
            continue;
        }
        cursor_value = i + 1;
    }

    return std::string_view::npos;
}

// True if an LF at or after search_offset (and within max_http_header_bytes)
// is not preceded by CR. Within a field section such a line can never end the
// CRLF-delimited head; the caller passes the end of the accepted request-line
// or status-line, or a later offset whose earlier bytes were already scanned.
[[nodiscard]] inline bool has_http_bare_line_feed(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    for (auto line_feed = buffer.find('\n', std::max<std::size_t>(search_offset, 1)); line_feed != std::string_view::npos; line_feed = buffer.find('\n', line_feed + 1)) {
        if (buffer[line_feed - 1] != '\r') {
            return true;
        }
    }
    return false;
}

enum class http1_version_parse_result : std::uint8_t { invalid,
    unsupported,
    http10,
    http11 };

// HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3). A higher
// HTTP/1 minor version is processed as HTTP/1.1 (RFC 9110 section 6.2).
[[nodiscard]] inline http1_version_parse_result parse_http1_version(std::string_view version) noexcept {
    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' || version[5] > '9' || version[6] != '.' || version[7] < '0' || version[7] > '9') {
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
[[nodiscard]] std::optional<error_type> parse_http1_content_length_field(std::string_view value, http_content_length_state& state) noexcept {
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
[[nodiscard]] std::optional<error_type> parse_http1_transfer_encoding_field(std::string_view value, http_transfer_encoding_state& state) {
    if (state.parse_field(value) == http_transfer_encoding_parse_status::malformed) {
        return error_type::invalid_transfer_encoding;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

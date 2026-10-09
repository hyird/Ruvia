#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/field/request_header_kind.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(
    request_header_kind kind) noexcept {
    switch (kind) {
        case request_header_kind::access_control_request_method:
        case request_header_kind::authorization:
        case request_header_kind::content_type:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::origin:
        case request_header_kind::range:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::user_agent:
            return 1U << static_cast<std::uint32_t>(kind);
        case request_header_kind::other:
        case request_header_kind::accept:
        case request_header_kind::accept_encoding:
        case request_header_kind::access_control_request_headers:
        case request_header_kind::connection:
        case request_header_kind::content_encoding:
        case request_header_kind::content_length:
        case request_header_kind::cookie:
        case request_header_kind::expect:
        case request_header_kind::host:
        case request_header_kind::if_match:
        case request_header_kind::if_none_match:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::transfer_encoding:
        case request_header_kind::upgrade:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
            return 0;
    }
    return 0;
}

enum class chunk_size_line_status : std::uint8_t {
    ok,
    invalid_size,
    overflow,
    invalid_extension,
};

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces multi-comparison chains and lets scan loops validate as they move.
inline constexpr std::array<bool, 256> http_token_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table_value[c] = true;
    }
    for (const unsigned char c :
        {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table_value[c] = true;
    }
    return table_value;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> http_field_value_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = 0; c < 256; ++c) {
        table_value[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table_value;
}();

[[nodiscard]] inline bool is_http_token_char(unsigned char c) noexcept {
    return http_token_char_table[c];
}

[[nodiscard]] inline bool is_http_field_value_char(unsigned char c) noexcept {
    return http_field_value_char_table[c];
}

// Find the first non-token byte. Independent table loads let valid runs avoid
// one loop branch per byte; the final scan preserves the exact stop position.
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= 4) {
        const auto* bytes_value = value.data() + index;
        if (!(is_http_token_char(static_cast<unsigned char>(bytes_value[0])) &
                is_http_token_char(static_cast<unsigned char>(bytes_value[1])) &
                is_http_token_char(static_cast<unsigned char>(bytes_value[2])) &
                is_http_token_char(static_cast<unsigned char>(bytes_value[3])))) {
            break;
        }
        index += 4;
    }
    while (index < value.size() && is_http_token_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

[[nodiscard]] inline bool is_valid_http_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    return http_token_prefix_size(name) == name.size();
}

// Keep bulk-scan register use outside inlined short-value callers.
[[nodiscard]] bool is_valid_long_http_field_value_bytes(
    const char* data, std::size_t size) noexcept;

// Byte repertoire only. Protocol boundaries separately decide whether leading
// or trailing OWS is permitted (HTTP/3 currently accepts it, HTTP/2 does not).
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    if (value.size() > 32) {
        return is_valid_long_http_field_value_bytes(value.data(), value.size());
    }
    // Preserve a compile-time loop bound for short-value unrolling.
    const auto size = std::min<std::size_t>(32, value.size());
    for (std::size_t index = 0; index < size; ++index) {
        if (!is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// Normalized field values exclude leading and trailing OWS. Wire parsers
// that accept field-line OWS trim it before using this shared contract.
[[nodiscard]] inline bool is_valid_http_field_value(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    const auto first = static_cast<unsigned char>(value.front());
    const auto last = static_cast<unsigned char>(value.back());
    if (first == ' ' || first == '\t' || last == ' ' || last == '\t') {
        return false;
    }
    return is_valid_http_field_value_bytes(value);
}

[[nodiscard]] request_header_kind classify_request_header(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_name(std::string_view name) noexcept;
[[nodiscard]] bool is_valid_http_header_value(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_chunk_extension(std::string_view value) noexcept;
[[nodiscard]] chunk_size_line_status parse_http_chunk_size_line(
    std::string_view value, std::size_t& size) noexcept;

}  // namespace ruvia::detail

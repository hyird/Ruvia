#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

// The single owner of HTTP field-line syntax (RFC 9110 section 5): token and
// field-content byte classes and the normalized field name/value predicates.
// Field-specific grammars build on these; field-line OWS belongs to wire
// parsers, so normalized values never start or end with SP or HTAB.

namespace ruvia {

// Removes only SP and HTAB from the ends of an HTTP field value (OWS,
// RFC 9110 section 5.6.3). The result borrows the caller's input.
[[nodiscard]] constexpr std::string_view http_trim_ows(borrowed_text text) noexcept {
    auto value = text.view();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

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

[[nodiscard]] constexpr bool is_http_token_char(unsigned char c) noexcept {
    return http_token_char_table[c];
}

[[nodiscard]] constexpr bool is_http_field_value_char(unsigned char c) noexcept {
    return http_field_value_char_table[c];
}

// Find the first non-token byte. Independent table loads let valid runs avoid
// one loop branch per byte; the final scan preserves the exact stop position.
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= 4) {
        const auto* bytes_value = value.data() + index;
        // Plain bool operands keep the bitwise '&' branchless without the
        // short-circuit warning on call operands.
        const bool first = is_http_token_char(static_cast<unsigned char>(bytes_value[0]));
        const bool second = is_http_token_char(static_cast<unsigned char>(bytes_value[1]));
        const bool third = is_http_token_char(static_cast<unsigned char>(bytes_value[2]));
        const bool fourth = is_http_token_char(static_cast<unsigned char>(bytes_value[3]));
        if (!(first & second & third & fourth)) {
            break;
        }
        index += 4;
    }
    while (index < value.size() && is_http_token_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

namespace detail {
// Keep bulk-scan register use outside inlined short-value callers.
[[nodiscard]] bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept;
}  // namespace detail

// Byte repertoire only. Field parsing separately handles leading/trailing OWS.
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    if (value.size() > 32) {
        return detail::is_valid_long_http_field_value_bytes(value.data(), value.size());
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

// field-name = token.
[[nodiscard]] inline bool is_valid_http_header_name(std::string_view name) noexcept {
    return !name.empty() && http_token_prefix_size(name) == name.size();
}

// A normalized field value: field-content bytes without leading or trailing
// OWS. Wire parsers that accept field-line OWS trim it before this check.
[[nodiscard]] inline bool is_valid_http_header_value(std::string_view value) noexcept {
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

}  // namespace ruvia

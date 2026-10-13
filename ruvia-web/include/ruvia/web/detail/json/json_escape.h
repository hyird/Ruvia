#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/json/json_byte_scan.h"

namespace ruvia::detail {

enum class json_hex_case : std::uint8_t { upper,
    lower };

// Reservation hint for append_json_string. It counts escapes only; output for
// ill-formed UTF-8 grows by up to two bytes per replaced byte beyond the hint.
[[nodiscard]] inline std::size_t json_string_size_hint(std::string_view value) noexcept {
    std::size_t size = value.size() + 2;
    std::size_t offset = 0;
    while ((offset = find_json_escape_byte(value, offset)) != std::string_view::npos) {
        const auto c = static_cast<unsigned char>(value[offset]);
        switch (c) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                ++size;
                break;
            default:
                size += 5;
                break;
        }
        ++offset;
    }
    return size;
}

// Escapes and appends a JSON string. RFC 8259 §8.1 requires interchanged JSON
// to be UTF-8, so the output is well-formed UTF-8 whatever the input bytes are:
// well-formed multibyte sequences are copied verbatim, and each maximal subpart
// of an ill-formed sequence (a stray continuation byte, a truncated or overlong
// sequence, a surrogate, a value above U+10FFFF, or text in another encoding
// such as a Windows ANSI code page) becomes one U+FFFD REPLACEMENT CHARACTER
// (Unicode 3.9 "U+FFFD substitution of maximal subparts"). ASCII runs are still
// scanned 16 bytes at a time.
template <json_hex_case case_value = json_hex_case::upper, typename string_t_type>
inline void append_json_string(string_t_type& output, std::string_view value) {
    constexpr std::string_view replacement_character = "\xEF\xBF\xBD";
    output.push_back('"');
    std::size_t chunk_begin = 0;
    std::size_t i = 0;
    while ((i = find_json_string_token_byte(value, i)) != std::string_view::npos) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 0x80) {
            const auto sequence = scan_json_utf8_sequence(value, i);
            if (!sequence.well_formed_) {
                if (i > chunk_begin) {
                    output.append(value.data() + chunk_begin, i - chunk_begin);
                }
                output.append(replacement_character.data(), replacement_character.size());
                chunk_begin = i + sequence.length_;
            }
            i += sequence.length_;
            continue;
        }
        if (i > chunk_begin) {
            output.append(value.data() + chunk_begin, i - chunk_begin);
        }
        switch (c) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\b':
                output.append("\\b");
                break;
            case '\f':
                output.append("\\f");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                output.append("\\u00");
                if constexpr (case_value == json_hex_case::upper) {
                    output.push_back(http_upper_hex_digit(c >> 4));
                    output.push_back(http_upper_hex_digit(c));
                } else {
                    output.push_back(http_lower_hex_digit(c >> 4));
                    output.push_back(http_lower_hex_digit(c));
                }
                break;
        }
        chunk_begin = i + 1;
        ++i;
    }
    if (chunk_begin < value.size()) {
        output.append(value.data() + chunk_begin, value.size() - chunk_begin);
    }
    output.push_back('"');
}

}  // namespace ruvia::detail

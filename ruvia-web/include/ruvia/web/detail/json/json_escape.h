#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/web/detail/json/json_byte_scan.h"

namespace ruvia::detail {

enum class json_hex_case : std::uint8_t { upper,
    lower };

template <json_hex_case case_value = json_hex_case::upper>
[[nodiscard]] inline char json_hex_digit(std::uint8_t value) noexcept {
    constexpr char first_letter = case_value == json_hex_case::upper ? 'A' : 'a';
    return static_cast<char>(value < 10 ? ('0' + value) : (first_letter + value - 10));
}

[[nodiscard]] inline bool json_needs_escape(unsigned char value) noexcept {
    return value == '"' || value == '\\' || value < 0x20;
}

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

// Escapes and appends a JSON string. Bytes >= 0x20 (other than '"' and '\\')
// are copied verbatim, including any 0x80..0xFF, without UTF-8 validation: this
// is the response hot path, and validating every byte of every string is a cost
// the framework deliberately avoids. RFC 8259 8.1 requires interchanged JSON to
// be UTF-8, so the caller must supply valid UTF-8 in string values; an ill-formed
// sequence is emitted as-is and yields a non-UTF-8 body.
template <json_hex_case case_value = json_hex_case::upper, typename string_t_type>
inline void append_json_string(string_t_type& output, std::string_view value) {
    output.push_back('"');
    std::size_t chunk_begin = 0;
    std::size_t i = 0;
    while ((i = find_json_escape_byte(value, i)) != std::string_view::npos) {
        const auto c = static_cast<unsigned char>(value[i]);
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
                output.push_back(json_hex_digit<case_value>(static_cast<std::uint8_t>(c >> 4)));
                output.push_back(json_hex_digit<case_value>(static_cast<std::uint8_t>(c & 0x0F)));
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

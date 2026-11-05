#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/json/json_byte_scan.h"

namespace ruvia::detail {

// O(1) reservation hint for append_json_string, avoiding a pre-scan pass over
// the string before escaping.
[[nodiscard]] constexpr std::size_t json_string_size_hint(std::string_view value) noexcept {
    return value.size() + 2;
}

// Whether RFC 8259 §7 forbids this ASCII byte unescaped inside a string.
[[nodiscard]] constexpr bool json_byte_needs_escape(unsigned char byte) noexcept {
    return byte < 0x20 || byte == '"' || byte == '\\';
}

// Appends the escape of one byte for which json_byte_needs_escape holds: the
// two-character form where RFC 8259 §7 defines one, otherwise lowercase \u00xx
// (as ECMAScript JSON.stringify). The single owner of JSON escape spelling for
// both runtime strings and compile-time model keys.
template <typename output_type>
constexpr void append_json_escape(output_type& output, unsigned char byte) {
    switch (byte) {
        case '"':
            output.append(std::string_view("\\\""));
            break;
        case '\\':
            output.append(std::string_view("\\\\"));
            break;
        case '\b':
            output.append(std::string_view("\\b"));
            break;
        case '\f':
            output.append(std::string_view("\\f"));
            break;
        case '\n':
            output.append(std::string_view("\\n"));
            break;
        case '\r':
            output.append(std::string_view("\\r"));
            break;
        case '\t':
            output.append(std::string_view("\\t"));
            break;
        default:
            output.append(std::string_view("\\u00"));
            output.push_back(http_lower_hex_digit(byte >> 4));
            output.push_back(http_lower_hex_digit(byte));
            break;
    }
}

// Escapes and appends a JSON string. RFC 8259 §8.1 requires interchanged JSON
// to be UTF-8, so the output is well-formed UTF-8 whatever the input bytes are:
// well-formed multibyte sequences are copied verbatim, and each maximal subpart
// of an ill-formed sequence (a stray continuation byte, a truncated or overlong
// sequence, a surrogate, a value above U+10FFFF, or text in another encoding
// such as a Windows ANSI code page) becomes one U+FFFD REPLACEMENT CHARACTER
// (Unicode 3.9 "U+FFFD substitution of maximal subparts"). ASCII runs are still
// scanned 16 bytes at a time.
template <typename string_t_type>
inline void append_json_string(string_t_type& output, std::string_view value) {
    constexpr std::string_view replacement_character = "\xEF\xBF\xBD";
    output.push_back('"');
    std::size_t chunk_begin = 0;
    std::size_t i = 0;
    while ((i = find_json_string_token_byte(value, i)) != std::string_view::npos) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 0x80) {
            const auto sequence = scan_utf8_sequence(value, i);
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
        append_json_escape(output, c);
        chunk_begin = i + 1;
        ++i;
    }
    if (chunk_begin < value.size()) {
        output.append(value.data() + chunk_begin, value.size() - chunk_begin);
    }
    output.push_back('"');
}

// Appends `,"<name>":` with `name` JSON-escaped, for object member keys known
// at compile time. Returns false when `name` is not well-formed UTF-8: the
// runtime writer would substitute U+FFFD, so the parser, which compares decoded
// keys with the declared name, could never read such a member back.
template <typename output_type>
[[nodiscard]] constexpr bool append_json_member_key(output_type& output, std::string_view name) {
    output.push_back(',');
    output.push_back('"');
    for (std::size_t i = 0; i < name.size();) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte >= 0x80) {
            const auto sequence = scan_utf8_sequence(name, i);
            if (!sequence.well_formed_) {
                return false;
            }
            output.append(name.substr(i, sequence.length_));
            i += sequence.length_;
            continue;
        }
        if (json_byte_needs_escape(byte)) {
            append_json_escape(output, byte);
        } else {
            output.push_back(name[i]);
        }
        ++i;
    }
    output.push_back('"');
    output.push_back(':');
    return true;
}

}  // namespace ruvia::detail

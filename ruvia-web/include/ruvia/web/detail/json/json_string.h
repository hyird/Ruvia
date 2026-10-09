#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/hex.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/json/json_byte_scan.h"
#include "ruvia/web/detail/json/json_lex.h"

namespace ruvia::detail {

// Read exactly four hex digits as a UTF-16 code unit. Single owner of \uXXXX
// digit decoding for both the validation scan (parse_json_string) and the
// decode pass (decode_json_string).
[[nodiscard]] inline bool read_json_hex4(std::string_view input, std::uint32_t& value) noexcept {
    if (input.size() < 4) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hex = ::ruvia::decode_hex_nibble(input[i]);
        if (hex < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(hex);
    }
    return true;
}

[[nodiscard]] inline bool is_json_high_surrogate(std::uint32_t code_unit) noexcept {
    return code_unit >= 0xD800 && code_unit <= 0xDBFF;
}

[[nodiscard]] inline bool is_json_low_surrogate(std::uint32_t code_unit) noexcept {
    return code_unit >= 0xDC00 && code_unit <= 0xDFFF;
}

// Length (1-4) of the well-formed UTF-8 sequence beginning at input[i] (whose
// lead byte is >= 0x80), or 0 if the bytes there are not valid UTF-8. Enforces the
// Unicode 3.9 Table 3-7 constraints: no overlong forms (C0/C1, E0 80-9F, F0 80-8F),
// no UTF-16 surrogate code points (ED A0-BF), nothing above U+10FFFF (F4 90-.., F5+),
// and no stray/truncated continuation bytes. RFC 8259 §8.1 requires JSON to be UTF-8,
// so a string carrying anything else is rejected rather than passed through verbatim.
[[nodiscard]] inline std::size_t json_utf8_sequence_length(
    std::string_view input, std::size_t i) noexcept {
    const auto is_cont = [](unsigned char b) noexcept { return (b & 0xC0U) == 0x80U; };
    const auto b0 = static_cast<unsigned char>(input[i]);
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return (i + 1 < input.size() && is_cont(static_cast<unsigned char>(input[i + 1]))) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (i + 2 >= input.size()) {
            return 0;
        }
        const auto b1 = static_cast<unsigned char>(input[i + 1]);
        const auto b2 = static_cast<unsigned char>(input[i + 2]);
        if (!is_cont(b2)) {
            return 0;
        }
        if (b0 == 0xE0   ? (b1 < 0xA0 || b1 > 0xBF)
            : b0 == 0xED ? (b1 < 0x80 || b1 > 0x9F)
                         : !is_cont(b1)) {
            return 0;
        }
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (i + 3 >= input.size()) {
            return 0;
        }
        const auto b1 = static_cast<unsigned char>(input[i + 1]);
        const auto b2 = static_cast<unsigned char>(input[i + 2]);
        const auto b3 = static_cast<unsigned char>(input[i + 3]);
        if (!is_cont(b2) || !is_cont(b3)) {
            return 0;
        }
        if (b0 == 0xF0   ? (b1 < 0x90 || b1 > 0xBF)
            : b0 == 0xF4 ? (b1 < 0x80 || b1 > 0x8F)
                         : !is_cont(b1)) {
            return 0;
        }
        return 4;
    }
    return 0;  // 0x80-0xC1 (bare continuation / overlong lead) or 0xF5-0xFF
}

enum class json_string_encoding : std::uint8_t { literal,
    escaped };

class json_string_token final {
public:
    [[nodiscard]] std::string_view raw() const noexcept {
        return raw_;
    }

    [[nodiscard]] json_string_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    friend std::optional<json_string_token> parse_json_string(std::string_view& input) noexcept;

    json_string_token(std::string_view raw, json_string_encoding encoding) noexcept
        : raw_(raw),
          encoding_(encoding) {}

    std::string_view raw_;
    json_string_encoding encoding_;
};

// Scans one JSON string and commits the input cursor only after the closing
// quote and all encoded bytes have been validated. The token owns both pieces
// of scan state, so callers cannot observe a raw view without its encoding.
[[nodiscard]] inline std::optional<json_string_token> parse_json_string(
    std::string_view& input) noexcept {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (remaining.empty() || remaining.front() != '"') {
        return std::nullopt;
    }
    remaining.remove_prefix(1);

    auto encoding = json_string_encoding::literal;
    const char* const begin = remaining.data();
    for (std::size_t i = 0; i < remaining.size();) {
        i = find_json_string_token_byte(remaining, i);
        if (i == std::string_view::npos) {
            return std::nullopt;
        }
        const char c = remaining[i];
        if (c == '\\') {
            encoding = json_string_encoding::escaped;
            if (i + 1 >= remaining.size()) {
                return std::nullopt;
            }
            const char escape = remaining[i + 1];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') {
                i += 2;
                continue;
            }
            if (escape == 'u') {
                std::uint32_t code_unit = 0;
                if (i + 5 >= remaining.size() || !read_json_hex4(remaining.substr(i + 2), code_unit)) {
                    return std::nullopt;
                }
                if (is_json_high_surrogate(code_unit)) {
                    std::uint32_t low = 0;
                    if (i + 11 >= remaining.size() || remaining[i + 6] != '\\' ||
                        remaining[i + 7] != 'u' || !read_json_hex4(remaining.substr(i + 8), low) ||
                        !is_json_low_surrogate(low)) {
                        return std::nullopt;
                    }
                    i += 12;
                    continue;
                }
                if (is_json_low_surrogate(code_unit)) {
                    return std::nullopt;
                }
                i += 6;
                continue;
            }
            return std::nullopt;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            return std::nullopt;
        }
        if (c == '"') {
            const json_string_token token(std::string_view(begin, i), encoding);
            remaining.remove_prefix(i + 1);
            input = remaining;
            return token;
        }
        if (uc >= 0x80) {
            const auto length = json_utf8_sequence_length(remaining, i);
            if (length == 0) {
                return std::nullopt;
            }
            i += length;
            continue;
        }
        ++i;
    }

    return std::nullopt;
}

template <typename output_t_type>
void append_utf8(output_t_type& output, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

// Returns the complete decoded string or no value for malformed escape/UTF-8
// input. Mutable caller storage is intentionally not accepted: a failure must
// never expose the prefix produced before the malformed byte sequence.
[[nodiscard]] inline std::optional<std::pmr::string> decode_json_string(
    std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string output(pmr_resource_or_default(resource));
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c != '\\') {
            const auto uc = static_cast<unsigned char>(c);
            if (uc < 0x20) {
                return std::nullopt;
            }
            if (uc >= 0x80) {
                const auto length = json_utf8_sequence_length(input, i);
                if (length == 0) {
                    return std::nullopt;
                }
                for (std::size_t k = 0; k < length; ++k) {
                    output.push_back(input[i + k]);
                }
                i += length - 1;  // the loop's ++i steps past the final byte
                continue;
            }
            output.push_back(c);
            continue;
        }

        if (i + 1 >= input.size()) {
            return std::nullopt;
        }
        const char escape = input[++i];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!read_json_hex4(input.substr(i + 1), code_point)) {
                    return std::nullopt;
                }
                i += 4;
                if (is_json_high_surrogate(code_point)) {
                    if (i + 6 >= input.size() || input[i + 1] != '\\' || input[i + 2] != 'u') {
                        return std::nullopt;
                    }
                    std::uint32_t low = 0;
                    if (!read_json_hex4(input.substr(i + 3), low) || !is_json_low_surrogate(low)) {
                        return std::nullopt;
                    }
                    i += 6;
                    code_point = 0x10000 + (((code_point - 0xD800) << 10) | (low - 0xDC00));
                } else if (is_json_low_surrogate(code_point)) {
                    return std::nullopt;
                }
                append_utf8(output, code_point);
                break;
            }
            default:
                return std::nullopt;
        }
    }
    return output;
}

}  // namespace ruvia::detail

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/json/json_byte_scan.h"
#include "ruvia/web/detail/json/json_lex.h"

namespace ruvia::detail {

// Read exactly four hex digits as a UTF-16 code unit.
[[nodiscard]] inline bool read_json_hex4(std::string_view input, std::uint32_t& value) noexcept {
    if (input.size() < 4) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hex = http_decode_hex_nibble(input[i]);
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
    template <typename escaped_budget_type>
    friend std::optional<json_string_token> parse_json_string(
        std::string_view& input, std::pmr::string* decoded, escaped_budget_type&& consume_escaped_bytes);
    json_string_token(std::string_view raw, json_string_encoding encoding) noexcept
        : raw_(raw),
          encoding_(encoding) {}

    std::string_view raw_;
    json_string_encoding encoding_;
};

// Scans one JSON string and commits the input cursor only after the closing
// quote and all encoded bytes have been validated. The token owns both pieces
// of scan state, so callers cannot observe a raw view without its encoding.
//
// A literal string is read through raw() and never touches `decoded`. When
// `decoded` is non-null and the string contains an escape, the same pass
// replaces its contents with the decoded text, starting at the first escape,
// so escaped input is neither rescanned nor copied before it is known to need
// decoding. After a failed scan `decoded` holds unspecified contents.
template <typename escaped_budget_type>
[[nodiscard]] inline std::optional<json_string_token> parse_json_string(
    std::string_view& input, std::pmr::string* decoded, escaped_budget_type&& consume_escaped_bytes) {
    auto remaining = input;
    skip_json_whitespace(remaining);
    if (remaining.empty() || remaining.front() != '"') {
        return std::nullopt;
    }
    remaining.remove_prefix(1);

    auto encoding = json_string_encoding::literal;
    // Raw bytes before this offset are already represented in *decoded.
    std::size_t decoded_until = 0;
    for (std::size_t i = 0; i < remaining.size();) {
        i = find_json_string_token_byte(remaining, i);
        if (i == std::string_view::npos) {
            return std::nullopt;
        }
        const char c = remaining[i];
        if (c == '\\') {
            if (encoding == json_string_encoding::literal && decoded != nullptr) {
                decoded->clear();
            }
            encoding = json_string_encoding::escaped;
            if (i + 1 >= remaining.size()) {
                return std::nullopt;
            }
            std::uint32_t code_point = 0;
            std::size_t escape_length = 2;
            switch (const char escape = remaining[i + 1]) {
                case '"':
                case '\\':
                case '/':
                    code_point = static_cast<unsigned char>(escape);
                    break;
                case 'b':
                    code_point = '\b';
                    break;
                case 'f':
                    code_point = '\f';
                    break;
                case 'n':
                    code_point = '\n';
                    break;
                case 'r':
                    code_point = '\r';
                    break;
                case 't':
                    code_point = '\t';
                    break;
                case 'u': {
                    if (i + 5 >= remaining.size() || !read_json_hex4(remaining.substr(i + 2), code_point)) {
                        return std::nullopt;
                    }
                    escape_length = 6;
                    if (is_json_high_surrogate(code_point)) {
                        std::uint32_t low = 0;
                        if (i + 11 >= remaining.size() || remaining[i + 6] != '\\' ||
                            remaining[i + 7] != 'u' || !read_json_hex4(remaining.substr(i + 8), low) ||
                            !is_json_low_surrogate(low)) {
                            return std::nullopt;
                        }
                        code_point = 0x10000 + (((code_point - 0xD800) << 10) | (low - 0xDC00));
                        escape_length = 12;
                    } else if (is_json_low_surrogate(code_point)) {
                        return std::nullopt;
                    }
                    break;
                }
                default:
                    return std::nullopt;
            }
            const auto next_until = i + escape_length;
            if (decoded != nullptr) {
                if (!consume_escaped_bytes(next_until - decoded_until)) {
                    return std::nullopt;
                }
                decoded->append(remaining.data() + decoded_until, i - decoded_until);
                append_utf8(*decoded, code_point);
            }
            i = next_until;
            decoded_until = i;
            continue;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            return std::nullopt;
        }
        if (c == '"') {
            if (encoding == json_string_encoding::escaped && decoded != nullptr) {
                if (!consume_escaped_bytes(i - decoded_until)) {
                    return std::nullopt;
                }
                decoded->append(remaining.data() + decoded_until, i - decoded_until);
            }
            const json_string_token token(remaining.substr(0, i), encoding);
            remaining.remove_prefix(i + 1);
            input = remaining;
            return token;
        }
        if (uc >= 0x80) {
            const auto sequence = scan_utf8_sequence(remaining, i);
            if (!sequence.well_formed_) {
                return std::nullopt;
            }
            i += sequence.length_;
            continue;
        }
        ++i;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::optional<json_string_token> parse_json_string(
    std::string_view& input, std::pmr::string* decoded = nullptr) {
    return parse_json_string(input, decoded, [](std::size_t) noexcept { return true; });
}
// The text of a string scanned by parse_json_string(input, &decoded).
[[nodiscard]] inline std::string_view json_string_text(
    const json_string_token& token, const std::pmr::string& decoded) noexcept {
    return token.encoding() == json_string_encoding::literal ? token.raw() : std::string_view(decoded);
}

}  // namespace ruvia::detail

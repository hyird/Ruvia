#pragma once

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RUVIA_JSON_SCAN_SSE2 1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define RUVIA_JSON_SCAN_NEON 1
#endif

#include <concepts>
#include <utility>

#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_utf8.h"
#include "ruvia/web/detail/decimal_number.h"

namespace ruvia::detail {

inline constexpr std::size_t max_json_depth = 64;

inline void skip_json_whitespace(std::string_view& input) noexcept {
    while (!input.empty()) {
        const char c = input.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return;
        }
        input.remove_prefix(1);
    }
}

[[nodiscard]] inline bool consume_json_char(std::string_view& input, char expected) noexcept {
    skip_json_whitespace(input);
    if (input.empty() || input.front() != expected) {
        return false;
    }
    input.remove_prefix(1);
    return true;
}

[[nodiscard]] inline bool consume_json_literal(
    std::string_view& input, std::string_view literal) noexcept {
    skip_json_whitespace(input);
    if (!input.starts_with(literal)) {
        return false;
    }
    input.remove_prefix(literal.size());
    return true;
}

[[nodiscard]] inline std::size_t find_json_string_token_byte(
    std::string_view input, std::size_t offset) noexcept {
    if (offset >= input.size()) {
        return std::string_view::npos;
    }

#if defined(RUVIA_JSON_SCAN_SSE2)
    const auto quote = _mm_set1_epi8('"');
    const auto backslash = _mm_set1_epi8('\\');
    const auto sign_bit = _mm_set1_epi8(static_cast<char>(-128));
    const auto control_limit = _mm_set1_epi8(static_cast<char>(-96));
    while (offset + 16 <= input.size()) {
        const auto bytes_value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input.data() + offset));
        const auto unsigned_bytes = _mm_xor_si128(bytes_value, sign_bit);
        auto special = _mm_or_si128(_mm_cmpeq_epi8(bytes_value, quote), _mm_cmpeq_epi8(bytes_value, backslash));
        special = _mm_or_si128(special, _mm_cmplt_epi8(unsigned_bytes, control_limit));
        const auto mask =
            static_cast<unsigned>(_mm_movemask_epi8(special)) | static_cast<unsigned>(_mm_movemask_epi8(bytes_value));
        if (mask != 0) {
            return offset + std::countr_zero(mask);
        }
        offset += 16;
    }
#elif defined(RUVIA_JSON_SCAN_NEON)
    const auto quote = vdupq_n_u8(static_cast<std::uint8_t>('"'));
    const auto backslash = vdupq_n_u8(static_cast<std::uint8_t>('\\'));
    const auto control_limit = vdupq_n_u8(0x20);
    const auto high_bit_limit = vdupq_n_u8(0x80);
    while (offset + 16 <= input.size()) {
        const auto bytes_value = vld1q_u8(reinterpret_cast<const std::uint8_t*>(input.data() + offset));
        auto special = vorrq_u8(vceqq_u8(bytes_value, quote), vceqq_u8(bytes_value, backslash));
        special = vorrq_u8(special, vcltq_u8(bytes_value, control_limit));
        special = vorrq_u8(special, vcgeq_u8(bytes_value, high_bit_limit));
        if (vmaxvq_u8(special) != 0) {
            for (std::size_t index = 0; index < 16; ++index) {
                const auto byte = static_cast<unsigned char>(input[offset + index]);
                if (byte == '"' || byte == '\\' || byte < 0x20 || byte >= 0x80) {
                    return offset + index;
                }
            }
        }
        offset += 16;
    }
#endif

    for (; offset < input.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(input[offset]);
        if (byte == '"' || byte == '\\' || byte < 0x20 || byte >= 0x80) {
            return offset;
        }
    }
    return std::string_view::npos;
}

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
            const auto sequence = ::ruvia::scan_utf8_sequence(value, i);
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
            const auto sequence = ::ruvia::scan_utf8_sequence(name, i);
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
        std::string_view& input, std::string* decoded, escaped_budget_type&& consume_escaped_bytes);
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
    std::string_view& input, std::string* decoded, escaped_budget_type&& consume_escaped_bytes) {
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
            const auto sequence = ::ruvia::scan_utf8_sequence(remaining, i);
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
    std::string_view& input, std::string* decoded = nullptr) {
    return parse_json_string(input, decoded, [](std::size_t) noexcept { return true; });
}

// The text of a string scanned by parse_json_string(input, &decoded).
[[nodiscard]] inline std::string_view json_string_text(
    const json_string_token& token, const std::string& decoded) noexcept {
    return token.encoding() == json_string_encoding::literal ? token.raw() : std::string_view(decoded);
}

struct json_number_token final {
    // Bytes of the RFC 8259 number at the start of the input; 0 when none.
    std::size_t length_{0};
    // Whether an integer or fraction digit is nonzero.
    bool nonzero_significand_{false};
};

[[nodiscard]] inline json_number_token scan_json_number_token(std::string_view input) noexcept {
    json_number_token token;
    std::size_t index = 0;
    const auto consume_digits = [&] {
        const auto begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            token.nonzero_significand_ = token.nonzero_significand_ || input[index] != '0';
            ++index;
        }
        return index - begin;
    };
    if (index < input.size() && input[index] == '-') {
        ++index;
    }
    if (index >= input.size()) {
        return {};
    }
    if (input[index] == '0') {
        ++index;
        if (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            return {};
        }
    } else if (input[index] >= '1' && input[index] <= '9') {
        (void)consume_digits();
    } else {
        return {};
    }

    if (index < input.size() && input[index] == '.') {
        ++index;
        if (consume_digits() == 0) {
            return {};
        }
    }
    if (index < input.size() && (input[index] == 'e' || input[index] == 'E')) {
        ++index;
        if (index < input.size() && (input[index] == '+' || input[index] == '-')) {
            ++index;
        }
        const auto exponent_begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == exponent_begin) {
            return {};
        }
    }
    token.length_ = index;
    return token;
}

[[nodiscard]] inline bool skip_json_number_token(std::string_view& input) noexcept {
    const auto length = scan_json_number_token(input).length_;
    if (length == 0) {
        return false;
    }
    input.remove_prefix(length);
    return true;
}

template <typename number_t_type>
[[nodiscard]] bool parse_json_number_value(std::string_view& input, number_t_type& value) {
    skip_json_whitespace(input);
    const auto token = scan_json_number_token(input);
    if (token.length_ == 0) {
        return false;
    }

    const auto number = input.substr(0, token.length_);
    if constexpr (std::is_floating_point_v<number_t_type>) {
        // The scan already enforced the RFC 8259 grammar, which excludes the
        // non-finite spellings; only range errors remain.
        const auto parsed_value = convert_decimal_number<number_t_type>(number, token.nonzero_significand_);
        if (!parsed_value || !std::isfinite(*parsed_value)) {
            return false;
        }
        value = *parsed_value;
    } else {
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
    }
    input.remove_prefix(token.length_);
    return true;
}

}  // namespace ruvia::detail

#if defined(RUVIA_JSON_SCAN_SSE2)
#undef RUVIA_JSON_SCAN_SSE2
#elif defined(RUVIA_JSON_SCAN_NEON)
#undef RUVIA_JSON_SCAN_NEON
#endif

// The single owner of the JSON array and object grammars, structural value
// skipping, and object field traversal. Skipping, typed model parsing and
// dynamic token traversal all walk containers through these functions.

namespace ruvia::detail {

// Consumes one array directly from input and lets the visitor consume each
// element from the same cursor, positioned at the element's first byte. The
// visitor returns false to reject the array. input advances past ']' only when
// the whole array is accepted; nested values visited by the caller belong at
// depth + 1.
template <typename visitor_type>
[[nodiscard]] bool consume_json_array_elements(std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '[')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == ']') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        skip_json_whitespace(remaining);
        if (!static_cast<bool>(visitor_ref(remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

// Consumes one object directly from input. For each member the visitor
// receives the scanned key token and consumes the value from the same cursor
// (positioned after ':'); it returns false to reject the object. When
// `decoded_key` is non-null, escaped keys are decoded into it by the key scan
// itself (see parse_json_string). input advances past '}' only when the whole
// object is accepted; nested values visited by the caller belong at depth + 1.
template <typename visitor_type, typename key_budget_type>
[[nodiscard]] bool consume_json_object_members(std::string_view& input, std::size_t depth,
    std::string* decoded_key, visitor_type&& visitor, key_budget_type&& consume_key_bytes) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '{')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == '}') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        const auto key = parse_json_string(remaining, decoded_key, consume_key_bytes);
        if (!key.has_value() || !consume_json_char(remaining, ':')) {
            return false;
        }
        if (!static_cast<bool>(visitor_ref(*key, remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == '}') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_members(
    std::string_view& input, std::size_t depth, std::string* decoded_key, visitor_type&& visitor) {
    return consume_json_object_members(
        input, depth, decoded_key, std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_array(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_array_elements(input, depth, [depth, &consume_element](std::string_view& element) noexcept {
        return consume_element() && skip_json_value(element, depth + 1, consume_element);
    });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_object(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_object_members(input, depth, nullptr,
        [depth, &consume_element](const json_string_token&, std::string_view& value) noexcept {
            return skip_json_value(value, depth + 1, consume_element);
        });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    if (depth > max_json_depth) {
        return false;
    }
    skip_json_whitespace(input);
    if (input.empty()) {
        return false;
    }

    switch (input.front()) {
        case '"':
            return parse_json_string(input).has_value();
        case '{':
            // skip_json_object/skip_json_array already add depth+1 for their child values; passing
            // depth+1 here too would double-count and enforce half the documented max_json_depth.
            return skip_json_object(input, depth, consume_element);
        case '[':
            return skip_json_array(input, depth, consume_element);
        case 't':
            return consume_json_literal(input, "true");
        case 'f':
            return consume_json_literal(input, "false");
        case 'n':
            return consume_json_literal(input, "null");
        default:
            return skip_json_number_token(input);
    }
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept {
    auto consume_element = []() noexcept { return true; };
    return skip_json_value(input, depth, consume_element);
}

enum class json_object_visit_result : unsigned char { complete,
    stopped,
    invalid };

// Consumes one object directly from input and lets the visitor consume each
// value from the same cursor, under the member's decoded key. RUVIA_MODEL uses
// this On-Demand-style traversal so a known value is parsed into its typed
// field without first scanning it to discover a raw slice and then parsing
// that slice again. consume_key_allocation is charged with the raw size of
// every escaped key before the visitor sees it.
template <typename visitor_type, typename key_allocation_type>
[[nodiscard]] bool consume_json_object_fields(std::string_view& input, std::size_t depth,
    visitor_type&& visitor, key_allocation_type&& consume_key_allocation) {
    auto& visitor_ref = visitor;
    std::string decoded_key;
    bool key_started = false;
    return consume_json_object_members(
        input, depth, &decoded_key,
        [&](const json_string_token& key, std::string_view& value_input) {
            key_started = false;
            if (key.encoding() == json_string_encoding::escaped) {
                return static_cast<bool>(visitor_ref(std::string_view(decoded_key), value_input));
            }
            return static_cast<bool>(visitor_ref(key.raw(), value_input));
        },
        [&](std::size_t bytes_value) noexcept {
            const bool first = !std::exchange(key_started, true);
            return consume_key_allocation(bytes_value, first);
        });
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(
    std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    return consume_json_object_fields(input, depth,
        std::forward<visitor_type>(visitor), [](std::size_t, bool) noexcept { return true; });
}

// Visits every field of one complete JSON object body as a raw value slice
// (which may carry leading whitespace). Values are skipped at depth 1, so the
// whole body honours max_json_depth. The visitor returns false to stop.
template <typename visitor_type>
    requires std::predicate<visitor_type&, std::string_view, std::string_view>
[[nodiscard]] json_object_visit_result visit_json_object_fields(
    std::string_view body, visitor_type&& visitor) {
    auto input = body;
    auto& visitor_ref = visitor;
    bool stopped = false;
    const bool consumed = consume_json_object_fields(input, 0,
        [&visitor_ref, &stopped](std::string_view key, std::string_view& value_input) {
            const auto value_start = value_input;
            if (!skip_json_value(value_input, 1)) {
                return false;
            }
            const auto value = value_start.substr(0, value_start.size() - value_input.size());
            stopped = !visitor_ref(key, value);
            return !stopped;
        });
    if (stopped) {
        return json_object_visit_result::stopped;
    }
    if (!consumed) {
        return json_object_visit_result::invalid;
    }
    skip_json_whitespace(input);
    return input.empty() ? json_object_visit_result::complete : json_object_visit_result::invalid;
}

}  // namespace ruvia::detail

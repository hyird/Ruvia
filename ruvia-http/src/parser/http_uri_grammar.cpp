#include "parser/http_uri_grammar.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <system_error>

#include "ruvia/http/detail/util/hex.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] constexpr bool is_uri_sub_delimiter(unsigned char byte) noexcept {
    switch (byte) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

inline constexpr std::array<bool, 256> reg_name_char_table = [] {
    std::array<bool, 256> table_value{};
    for (std::size_t value = 0; value < table_value.size(); ++value) {
        const auto byte = static_cast<unsigned char>(value);
        table_value[value] = is_unreserved_byte(byte) || is_uri_sub_delimiter(byte);
    }
    return table_value;
}();

[[nodiscard]] constexpr bool is_uri_userinfo_literal(unsigned char byte) noexcept {
    return reg_name_char_table[byte] || byte == ':';
}

[[nodiscard]] bool parse_ipv6_hex_group(std::string_view literal, std::size_t& offset) noexcept {
    std::size_t digits = 0;
    while (offset < literal.size() && digits < 4 && is_hex_digit(literal[offset])) {
        ++offset;
        ++digits;
    }
    return digits != 0;
}

template <typename is_allowed_type>
[[nodiscard]] bool is_valid_percent_encoded(
    std::string_view value, is_allowed_type is_allowed) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte == '%') {
            if (index + 2 >= value.size() || decode_hex_nibble(value[index + 1]) < 0 ||
                decode_hex_nibble(value[index + 2]) < 0) {
                return false;
            }
            index += 2;
        } else if (!is_allowed(byte)) {
            return false;
        }
    }
    return true;
}

}  // namespace

[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept {
    return is_uri_userinfo_literal(byte) || byte == '@';
}

std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept {
    if (value.empty()) {
        return std::errc::invalid_argument;
    }

    std::uint16_t parsed_value = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed_value);
    if (ec != std::errc{}) {
        return ec;
    }
    if (ptr != end) {
        return std::errc::invalid_argument;
    }
    return parsed_value;
}

[[nodiscard]] bool is_valid_uri_component(
    std::string_view value, bool allow_slash, bool allow_question) noexcept {
    return is_valid_percent_encoded(value, [allow_slash, allow_question](unsigned char byte) noexcept {
        return is_uri_pchar(byte) || (allow_slash && byte == '/') || (allow_question && byte == '?');
    });
}

[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept {
    return is_valid_percent_encoded(value, [](unsigned char byte) noexcept {
        return is_uri_userinfo_literal(byte);
    });
}

[[nodiscard]] bool is_valid_uri_port(std::string_view value) noexcept {
    return std::ranges::all_of(value, is_decimal_digit);
}

[[nodiscard]] bool parse_ipv4_address(std::string_view value) noexcept {
    std::size_t offset = 0;
    for (int part = 0; part < 4; ++part) {
        if (offset >= value.size() || !is_decimal_digit(value[offset])) {
            return false;
        }
        const auto part_begin = offset;
        unsigned int octet = 0;
        std::size_t digits = 0;
        while (offset < value.size() && is_decimal_digit(value[offset])) {
            octet = octet * 10 + static_cast<unsigned int>(value[offset] - '0');
            ++offset;
            ++digits;
            if (digits > 3 || octet > 255) {
                return false;
            }
        }
        if (digits > 1 && value[part_begin] == '0') {
            return false;
        }
        if (part == 3) {
            return offset == value.size();
        }
        if (offset >= value.size() || value[offset] != '.') {
            return false;
        }
        ++offset;
    }
    return false;
}

[[nodiscard]] bool is_valid_ipv6_literal(std::string_view literal) noexcept {
    if (literal.empty()) {
        return false;
    }

    std::size_t offset = 0;
    int groups = 0;
    bool compressed = false;

    if (literal.starts_with("::")) {
        compressed = true;
        offset = 2;
        if (offset == literal.size()) {
            return true;
        }
    }

    while (offset < literal.size()) {
        if (groups >= 8) {
            return false;
        }

        const auto next_dot = literal.find('.', offset);
        const auto next_colon = literal.find(':', offset);
        if (is_decimal_digit(literal[offset]) && next_dot != std::string_view::npos &&
            (next_colon == std::string_view::npos || next_dot < next_colon)) {
            if (!parse_ipv4_address(literal.substr(offset))) {
                return false;
            }
            groups += 2;
            offset = literal.size();
            break;
        }

        if (!parse_ipv6_hex_group(literal, offset)) {
            return false;
        }
        ++groups;

        if (offset == literal.size()) {
            break;
        }
        if (literal[offset] != ':') {
            return false;
        }
        if (offset + 1 < literal.size() && literal[offset + 1] == ':') {
            if (compressed) {
                return false;
            }
            compressed = true;
            offset += 2;
            if (offset == literal.size()) {
                break;
            }
        } else {
            ++offset;
            if (offset == literal.size()) {
                return false;
            }
        }
    }

    return compressed ? groups < 8 : groups == 8;
}

[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept {
    if (literal.size() < 4 || (literal.front() != 'v' && literal.front() != 'V')) {
        return false;
    }

    std::size_t cursor_value = 1;
    const auto version_begin = cursor_value;
    while (cursor_value < literal.size() && is_hex_digit(literal[cursor_value])) {
        ++cursor_value;
    }
    if (cursor_value == version_begin || cursor_value >= literal.size() || literal[cursor_value] != '.') {
        return false;
    }
    ++cursor_value;
    if (cursor_value == literal.size()) {
        return false;
    }

    literal.remove_prefix(cursor_value);
    for (const unsigned char byte : literal) {
        if (!is_uri_userinfo_literal(byte)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept {
    return !value.empty() &&
           is_valid_percent_encoded(value, [](unsigned char byte) noexcept {
               return reg_name_char_table[byte];
           });
}

}  // namespace ruvia::detail

#include "parser/http_serialized_origin.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <variant>

#include "parser/http_uri_grammar.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool is_lower_alpha(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool is_lower_alpha_numeric(char value) noexcept {
    return is_lower_alpha(value) || is_decimal_digit(value);
}

[[nodiscard]] bool is_lower_hex_digit(char value) noexcept {
    return is_decimal_digit(value) || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool is_valid_serialized_origin_scheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !is_lower_alpha(scheme.front())) {
        return false;
    }
    for (const auto value : scheme.substr(1)) {
        if (!is_lower_alpha_numeric(value) && value != '+' && value != '-' && value != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_serialized_origin_ipv4_number(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (std::ranges::all_of(value, is_decimal_digit)) {
        return true;
    }
    if (value.size() >= 2 && value.front() == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return std::ranges::all_of(value.substr(2), [](char digit) noexcept {
            return is_decimal_digit(digit) || (digit >= 'a' && digit <= 'f') ||
                   (digit >= 'A' && digit <= 'F');
        });
    }
    if (value.size() >= 2 && value.front() == '0') {
        return std::ranges::all_of(
            value.substr(1), [](char digit) noexcept { return digit >= '0' && digit <= '7'; });
    }
    return false;
}

[[nodiscard]] bool serialized_origin_domain_ends_in_number(std::string_view domain) noexcept {
    if (!domain.empty() && domain.back() == '.') {
        domain.remove_suffix(1);
    }
    const auto separator = domain.rfind('.');
    const auto last_label =
        separator == std::string_view::npos ? domain : domain.substr(separator + 1);
    return is_serialized_origin_ipv4_number(last_label);
}

[[nodiscard]] bool is_serialized_origin_domain_byte(unsigned char byte) noexcept {
    if (byte >= 0x80 || byte <= 0x20 || byte == 0x7f || (byte >= 'A' && byte <= 'Z')) {
        return false;
    }
    switch (byte) {
        case '#':
        case '%':
        case '/':
        case ':':
        case '<':
        case '>':
        case '?':
        case '@':
        case '[':
        case '\\':
        case ']':
        case '^':
        case '|':
            return false;
        default:
            return true;
    }
}

[[nodiscard]] bool is_valid_serialized_origin_domain(std::string_view domain) noexcept {
    if (domain.empty()) {
        return false;
    }
    if (serialized_origin_domain_ends_in_number(domain)) {
        return false;
    }
    return std::ranges::all_of(domain, [](char byte) noexcept {
        return is_serialized_origin_domain_byte(static_cast<unsigned char>(byte));
    });
}

[[nodiscard]] std::optional<std::uint16_t> parse_serialized_origin_h16(
    std::string_view group) noexcept {
    if (group.empty() || group.size() > 4 || (group.size() > 1 && group.front() == '0')) {
        return std::nullopt;
    }
    std::uint16_t value = 0;
    for (const auto digit : group) {
        if (!is_lower_hex_digit(digit)) {
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>(
            value * 16U + (is_decimal_digit(digit) ? static_cast<unsigned>(digit - '0')
                                                   : static_cast<unsigned>(digit - 'a' + 10)));
    }
    return value;
}

[[nodiscard]] bool parse_serialized_origin_ipv6_groups(std::string_view side,
    std::array<std::uint16_t, 8>& pieces, std::size_t start, std::size_t& count) noexcept {
    count = 0;
    if (side.empty()) {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = side.find(':', offset);
        const auto group = side.substr(offset,
            separator == std::string_view::npos ? std::string_view::npos : separator - offset);
        const auto value = parse_serialized_origin_h16(group);
        if (!value.has_value() || start + count >= pieces.size()) {
            return false;
        }
        pieces[start + count] = *value;
        ++count;
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
        if (offset == side.size()) {
            return false;
        }
    }
}

[[nodiscard]] std::size_t find_serialized_origin_ipv6_compression_index(
    const std::array<std::uint16_t, 8>& pieces) noexcept {
    constexpr std::size_t no_compression = 8;
    std::size_t longest_index = no_compression;
    std::size_t longest_size = 1;
    std::size_t found_index = no_compression;
    std::size_t found_size = 0;

    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        if (pieces[piece_index] != 0) {
            if (found_size > longest_size) {
                longest_index = found_index;
                longest_size = found_size;
            }
            found_index = no_compression;
            found_size = 0;
            continue;
        }
        if (found_index == no_compression) {
            found_index = piece_index;
        }
        ++found_size;
    }
    if (found_size > longest_size) {
        return found_index;
    }
    return longest_index;
}

[[nodiscard]] bool append_serialized_origin_ipv6_hex(
    std::array<char, 39>& output, std::size_t& size, std::uint16_t value) noexcept {
    constexpr std::string_view hex = "0123456789abcdef";
    bool emitted = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFU);
        if (nibble == 0 && !emitted && shift != 0) {
            continue;
        }
        if (size == output.size()) {
            return false;
        }
        output[size++] = hex[nibble];
        emitted = true;
    }
    return true;
}

[[nodiscard]] bool append_serialized_origin_ipv6_byte(
    std::array<char, 39>& output, std::size_t& size, char byte) noexcept {
    if (size == output.size()) {
        return false;
    }
    output[size++] = byte;
    return true;
}

[[nodiscard]] bool is_canonical_serialized_origin_ipv6(
    std::string_view literal, const std::array<std::uint16_t, 8>& pieces) noexcept {
    std::array<char, 39> serialized{};
    std::size_t size = 0;
    const auto compression_index = find_serialized_origin_ipv6_compression_index(pieces);
    bool ignore_zero = false;

    for (std::size_t piece_index = 0; piece_index < pieces.size(); ++piece_index) {
        if (ignore_zero && pieces[piece_index] == 0) {
            continue;
        }
        if (ignore_zero) {
            ignore_zero = false;
        }
        if (compression_index == piece_index) {
            if (piece_index == 0) {
                if (!append_serialized_origin_ipv6_byte(serialized, size, ':')) {
                    return false;
                }
            }
            if (!append_serialized_origin_ipv6_byte(serialized, size, ':')) {
                return false;
            }
            ignore_zero = true;
            continue;
        }
        if (!append_serialized_origin_ipv6_hex(serialized, size, pieces[piece_index])) {
            return false;
        }
        if (piece_index != pieces.size() - 1 &&
            !append_serialized_origin_ipv6_byte(serialized, size, ':')) {
            return false;
        }
    }

    return literal.size() == size &&
           std::equal(serialized.begin(), serialized.begin() + static_cast<std::ptrdiff_t>(size),
               literal.begin());
}

[[nodiscard]] bool is_valid_serialized_origin_ipv6(std::string_view literal) noexcept {
    std::array<std::uint16_t, 8> pieces{};
    const auto compression = literal.find("::");
    if (compression == std::string_view::npos) {
        std::size_t groups = 0;
        return parse_serialized_origin_ipv6_groups(literal, pieces, 0, groups) &&
               groups == pieces.size() && is_canonical_serialized_origin_ipv6(literal, pieces);
    }
    if (literal.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    std::size_t left_groups = 0;
    std::size_t right_groups = 0;
    std::array<std::uint16_t, 8> right_pieces{};
    if (!parse_serialized_origin_ipv6_groups(literal.substr(0, compression), pieces, 0, left_groups) ||
        !parse_serialized_origin_ipv6_groups(
            literal.substr(compression + 2), right_pieces, 0, right_groups) ||
        left_groups + right_groups > 7) {
        return false;
    }
    for (std::size_t i = 0; i < right_groups; ++i) {
        pieces[pieces.size() - right_groups + i] = right_pieces[i];
    }
    return is_canonical_serialized_origin_ipv6(literal, pieces);
}

[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_serialized_origin_port(std::string_view value) noexcept {
    // A serialized URL port is the shortest decimal form of the URL record's
    // 16-bit port. Merely accepting five digits admits values such as 99999,
    // while accepting leading zeroes admits spellings no serializer can emit.
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return std::errc::invalid_argument;
    }
    return parse_port_value(value);
}

[[nodiscard]] std::optional<std::uint16_t> serialized_origin_default_port(
    std::string_view scheme) noexcept {
    if (scheme == "ftp") {
        return 21;
    }
    if (scheme == "http" || scheme == "ws") {
        return 80;
    }
    if (scheme == "https" || scheme == "wss") {
        return 443;
    }
    return std::nullopt;
}

}  // namespace

bool is_valid_http_serialized_origin(std::string_view value) noexcept {
    const auto scheme_end = value.find("://");
    const auto scheme = value.substr(0, scheme_end);
    if (scheme_end == std::string_view::npos || !is_valid_serialized_origin_scheme(scheme)) {
        return false;
    }

    const auto authority = value.substr(scheme_end + 3);
    if (authority.empty()) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool has_port = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return false;
        }
        host = authority.substr(1, close - 1);
        const auto remainder = authority.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return false;
            }
            has_port = true;
            port = remainder.substr(1);
        }
        if (!is_valid_serialized_origin_ipv6(host)) {
            return false;
        }
    } else {
        const auto port_separator = authority.find(':');
        if (port_separator == std::string_view::npos) {
            host = authority;
        } else {
            if (authority.find(':', port_separator + 1) != std::string_view::npos) {
                return false;
            }
            host = authority.substr(0, port_separator);
            has_port = true;
            port = authority.substr(port_separator + 1);
        }
        if (!parse_ipv4_address(host) && !is_valid_serialized_origin_domain(host)) {
            return false;
        }
    }
    if (!has_port) {
        return true;
    }
    const auto port_value = parse_serialized_origin_port(port);
    if ((port_value.index() != 0)) {
        return false;
    }
    const auto default_port = serialized_origin_default_port(scheme);
    return !default_port.has_value() || std::get<0>(port_value) != *default_port;
}

}  // namespace ruvia::detail

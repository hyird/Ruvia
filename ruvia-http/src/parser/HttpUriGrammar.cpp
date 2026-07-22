#include "ruvia/http/detail/parser/HttpUriGrammar.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <system_error>

#include "ruvia/http/detail/Hex.h"

namespace ruvia::detail {

namespace {

inline constexpr std::array<bool, 256> kRegNameCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table[c] = true;
    }
    for (const unsigned char c :
         {'-', '.', '_', '~', '!', '$', '&', '\'', '(', ')', '*', '+', ',', ';', '='}) {
        table[c] = true;
    }
    return table;
}();

[[nodiscard]] bool isHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'F') ||
        (c >= 'a' && c <= 'f');
}

[[nodiscard]] bool isUriUnreserved(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') ||
        (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') ||
        byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

[[nodiscard]] bool isUriSubDelimiter(unsigned char byte) noexcept {
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


[[nodiscard]] bool parseIpv6HexGroup(std::string_view literal, std::size_t& offset) noexcept {
    std::size_t digits = 0;
    while (offset < literal.size() && digits < 4 && isHexDigit(literal[offset])) {
        ++offset;
        ++digits;
    }
    return digits != 0;
}

}  // namespace

[[nodiscard]] bool isUriPchar(unsigned char byte) noexcept {
    return isUriUnreserved(byte) || isUriSubDelimiter(byte) ||
        byte == ':' || byte == '@';
}

[[nodiscard]] bool parsePortValue(std::string_view value, std::uint16_t& port) noexcept {
    if (value.empty()) {
        return false;
    }

    unsigned int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

[[nodiscard]] bool isValidUriComponent(
    std::string_view value,
    bool allowSlash,
    bool allowQuestion) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '%') {
            if (i + 2 >= value.size() ||
                decodeHexNibble(value[i + 1]) < 0 ||
                decodeHexNibble(value[i + 2]) < 0) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!isUriPchar(byte) &&
            !(allowSlash && byte == '/') &&
            !(allowQuestion && byte == '?')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidUriUserinfo(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == '%') {
            if (i + 2 >= value.size() ||
                decodeHexNibble(value[i + 1]) < 0 ||
                decodeHexNibble(value[i + 2]) < 0) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!isUriUnreserved(byte) && !isUriSubDelimiter(byte) &&
            byte != ':') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidUriPort(std::string_view value) noexcept {
    return std::ranges::all_of(value, isDecimalDigit);
}

[[nodiscard]] bool parseIpv4Address(std::string_view value) noexcept {
    std::size_t offset = 0;
    for (int part = 0; part < 4; ++part) {
        if (offset >= value.size() || !isDecimalDigit(value[offset])) {
            return false;
        }
        const auto partBegin = offset;
        unsigned int octet = 0;
        std::size_t digits = 0;
        while (offset < value.size() && isDecimalDigit(value[offset])) {
            octet = octet * 10 + static_cast<unsigned int>(value[offset] - '0');
            ++offset;
            ++digits;
            if (digits > 3 || octet > 255) {
                return false;
            }
        }
        if (digits > 1 && value[partBegin] == '0') {
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

[[nodiscard]] bool isValidIpv6Literal(std::string_view literal) noexcept {
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

        const auto nextDot = literal.find('.', offset);
        const auto nextColon = literal.find(':', offset);
        if (isDecimalDigit(literal[offset]) &&
            nextDot != std::string_view::npos &&
            (nextColon == std::string_view::npos || nextDot < nextColon)) {
            if (!parseIpv4Address(literal.substr(offset))) {
                return false;
            }
            groups += 2;
            offset = literal.size();
            break;
        }

        if (!parseIpv6HexGroup(literal, offset)) {
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

[[nodiscard]] bool isValidIpvFuture(std::string_view literal) noexcept {
    if (literal.size() < 4 || (literal.front() != 'v' && literal.front() != 'V')) {
        return false;
    }

    std::size_t cursor = 1;
    const auto versionBegin = cursor;
    while (cursor < literal.size() && isHexDigit(literal[cursor])) {
        ++cursor;
    }
    if (cursor == versionBegin || cursor >= literal.size() || literal[cursor] != '.') {
        return false;
    }
    ++cursor;
    if (cursor == literal.size()) {
        return false;
    }

    for (; cursor < literal.size(); ++cursor) {
        const auto byte = static_cast<unsigned char>(literal[cursor]);
        if (byte == ':' || kRegNameCharTable[byte]) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool isValidRegName(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte == '%') {
            if (index + 2 >= value.size() ||
                decodeHexNibble(value[index + 1]) < 0 ||
                decodeHexNibble(value[index + 2]) < 0) {
                return false;
            }
            index += 2;
            continue;
        }
        if (!kRegNameCharTable[byte]) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail

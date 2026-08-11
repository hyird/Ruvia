#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/detail/parser/HttpUriGrammar.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] bool isLowerAlpha(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool isLowerAlphaNumeric(char value) noexcept {
    return isLowerAlpha(value) || isDecimalDigit(value);
}

[[nodiscard]] bool isLowerHexDigit(char value) noexcept {
    return isDecimalDigit(value) || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool isValidSerializedOriginScheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !isLowerAlpha(scheme.front())) {
        return false;
    }
    for (const auto value : scheme.substr(1)) {
        if (!isLowerAlphaNumeric(value) && value != '+' && value != '-' && value != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isSerializedOriginIpv4Number(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (std::ranges::all_of(value, isDecimalDigit)) {
        return true;
    }
    if (value.size() >= 2 && value.front() == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return std::ranges::all_of(value.substr(2), [](char digit) noexcept {
            return isDecimalDigit(digit) || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
        });
    }
    if (value.size() >= 2 && value.front() == '0') {
        return std::ranges::all_of(value.substr(1), [](char digit) noexcept {
            return digit >= '0' && digit <= '7';
        });
    }
    return false;
}

[[nodiscard]] bool serializedOriginDomainEndsInNumber(std::string_view domain) noexcept {
    if (!domain.empty() && domain.back() == '.') {
        domain.remove_suffix(1);
    }
    const auto separator = domain.rfind('.');
    const auto lastLabel = separator == std::string_view::npos ? domain : domain.substr(separator + 1);
    return isSerializedOriginIpv4Number(lastLabel);
}

[[nodiscard]] bool isSerializedOriginDomainByte(unsigned char byte) noexcept {
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

[[nodiscard]] bool isValidSerializedOriginDomain(std::string_view domain) noexcept {
    if (domain.empty()) {
        return false;
    }
    if (serializedOriginDomainEndsInNumber(domain)) {
        return false;
    }
    return std::ranges::all_of(domain, [](char byte) noexcept {
        return isSerializedOriginDomainByte(static_cast<unsigned char>(byte));
    });
}

[[nodiscard]] std::optional<std::uint16_t> parseSerializedOriginH16(std::string_view group) noexcept {
    if (group.empty() || group.size() > 4 || (group.size() > 1 && group.front() == '0')) {
        return std::nullopt;
    }
    std::uint16_t value = 0;
    for (const auto digit : group) {
        if (!isLowerHexDigit(digit)) {
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>(value * 16U + (isDecimalDigit(digit) ? static_cast<unsigned>(digit - '0') : static_cast<unsigned>(digit - 'a' + 10)));
    }
    return value;
}

[[nodiscard]] bool parseSerializedOriginIpv6Groups(std::string_view side, std::array<std::uint16_t, 8>& pieces, std::size_t start, std::size_t& count) noexcept {
    count = 0;
    if (side.empty()) {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = side.find(':', offset);
        const auto group = side.substr(offset, separator == std::string_view::npos ? std::string_view::npos : separator - offset);
        const auto value = parseSerializedOriginH16(group);
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

[[nodiscard]] std::size_t findSerializedOriginIpv6CompressionIndex(const std::array<std::uint16_t, 8>& pieces) noexcept {
    constexpr std::size_t kNoCompression = 8;
    std::size_t longestIndex = kNoCompression;
    std::size_t longestSize = 1;
    std::size_t foundIndex = kNoCompression;
    std::size_t foundSize = 0;

    for (std::size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
        if (pieces[pieceIndex] != 0) {
            if (foundSize > longestSize) {
                longestIndex = foundIndex;
                longestSize = foundSize;
            }
            foundIndex = kNoCompression;
            foundSize = 0;
            continue;
        }
        if (foundIndex == kNoCompression) {
            foundIndex = pieceIndex;
        }
        ++foundSize;
    }
    if (foundSize > longestSize) {
        return foundIndex;
    }
    return longestIndex;
}

[[nodiscard]] bool appendSerializedOriginIpv6Hex(std::array<char, 39>& output, std::size_t& size, std::uint16_t value) noexcept {
    constexpr std::string_view kHex = "0123456789abcdef";
    bool emitted = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> shift) & 0xFU);
        if (nibble == 0 && !emitted && shift != 0) {
            continue;
        }
        if (size == output.size()) {
            return false;
        }
        output[size++] = kHex[nibble];
        emitted = true;
    }
    return true;
}

[[nodiscard]] bool appendSerializedOriginIpv6Byte(std::array<char, 39>& output, std::size_t& size, char byte) noexcept {
    if (size == output.size()) {
        return false;
    }
    output[size++] = byte;
    return true;
}

[[nodiscard]] bool isCanonicalSerializedOriginIpv6(std::string_view literal, const std::array<std::uint16_t, 8>& pieces) noexcept {
    std::array<char, 39> serialized{};
    std::size_t size = 0;
    const auto compressionIndex = findSerializedOriginIpv6CompressionIndex(pieces);
    bool ignoreZero = false;

    for (std::size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
        if (ignoreZero && pieces[pieceIndex] == 0) {
            continue;
        }
        if (ignoreZero) {
            ignoreZero = false;
        }
        if (compressionIndex == pieceIndex) {
            if (pieceIndex == 0) {
                if (!appendSerializedOriginIpv6Byte(serialized, size, ':')) {
                    return false;
                }
            }
            if (!appendSerializedOriginIpv6Byte(serialized, size, ':')) {
                return false;
            }
            ignoreZero = true;
            continue;
        }
        if (!appendSerializedOriginIpv6Hex(serialized, size, pieces[pieceIndex])) {
            return false;
        }
        if (pieceIndex != pieces.size() - 1 && !appendSerializedOriginIpv6Byte(serialized, size, ':')) {
            return false;
        }
    }

    return literal.size() == size && std::equal(serialized.begin(), serialized.begin() + static_cast<std::ptrdiff_t>(size), literal.begin());
}

[[nodiscard]] bool isValidSerializedOriginIpv6(std::string_view literal) noexcept {
    std::array<std::uint16_t, 8> pieces{};
    const auto compression = literal.find("::");
    if (compression == std::string_view::npos) {
        std::size_t groups = 0;
        return parseSerializedOriginIpv6Groups(literal, pieces, 0, groups) && groups == pieces.size() && isCanonicalSerializedOriginIpv6(literal, pieces);
    }
    if (literal.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    std::size_t leftGroups = 0;
    std::size_t rightGroups = 0;
    std::array<std::uint16_t, 8> rightPieces{};
    if (!parseSerializedOriginIpv6Groups(literal.substr(0, compression), pieces, 0, leftGroups) || !parseSerializedOriginIpv6Groups(literal.substr(compression + 2), rightPieces, 0, rightGroups) || leftGroups + rightGroups > 7) {
        return false;
    }
    for (std::size_t i = 0; i < rightGroups; ++i) {
        pieces[pieces.size() - rightGroups + i] = rightPieces[i];
    }
    return isCanonicalSerializedOriginIpv6(literal, pieces);
}

[[nodiscard]] bool parseSerializedOriginPort(std::string_view value, std::uint16_t& port) noexcept {
    // A serialized URL port is the shortest decimal form of the URL record's
    // 16-bit port. Merely accepting five digits admits values such as 99999,
    // while accepting leading zeroes admits spellings no serializer can emit.
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    return parsePortValue(value, port);
}

[[nodiscard]] std::optional<std::uint16_t> serializedOriginDefaultPort(std::string_view scheme) noexcept {
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

bool isValidHttpSerializedOrigin(std::string_view value) noexcept {
    const auto schemeEnd = value.find("://");
    const auto scheme = value.substr(0, schemeEnd);
    if (schemeEnd == std::string_view::npos || !isValidSerializedOriginScheme(scheme)) {
        return false;
    }

    const auto authority = value.substr(schemeEnd + 3);
    if (authority.empty()) {
        return false;
    }

    std::string_view host;
    std::string_view port;
    bool hasPort = false;
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
            hasPort = true;
            port = remainder.substr(1);
        }
        if (!isValidSerializedOriginIpv6(host)) {
            return false;
        }
    } else {
        const auto portSeparator = authority.find(':');
        if (portSeparator == std::string_view::npos) {
            host = authority;
        } else {
            if (authority.find(':', portSeparator + 1) != std::string_view::npos) {
                return false;
            }
            host = authority.substr(0, portSeparator);
            hasPort = true;
            port = authority.substr(portSeparator + 1);
        }
        if (!parseIpv4Address(host) && !isValidSerializedOriginDomain(host)) {
            return false;
        }
    }
    if (!hasPort) {
        return true;
    }
    std::uint16_t portValue = 0;
    if (!parseSerializedOriginPort(port, portValue)) {
        return false;
    }
    const auto defaultPort = serializedOriginDefaultPort(scheme);
    return !defaultPort.has_value() || portValue != *defaultPort;
}

}  // namespace ruvia::detail

#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

#include <array>
#include <charconv>
#include <system_error>

namespace ruvia::detail {
namespace {

// URI reg-name chars: unreserved / sub-delims (pct-encoded handled by caller).
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

[[nodiscard]] bool isDecimalDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

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

[[nodiscard]] bool isUriPchar(unsigned char byte) noexcept {
    return isUriUnreserved(byte) || isUriSubDelimiter(byte) ||
        byte == ':' || byte == '@';
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

[[nodiscard]] bool parseIpv6HexGroup(std::string_view literal, std::size_t& offset) noexcept {
    std::size_t digits = 0;
    while (offset < literal.size() && digits < 4 && isHexDigit(literal[offset])) {
        ++offset;
        ++digits;
    }
    return digits != 0;
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

[[nodiscard]] bool isUnreservedByte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') ||
        (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') ||
        byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

struct NormalizedHostUnit final {
    unsigned char byte{0};
    bool encodedReserved{false};
};

[[nodiscard]] bool nextNormalizedHostUnit(
    std::string_view value,
    std::size_t& cursor,
    NormalizedHostUnit& output) noexcept {
    if (cursor == value.size()) {
        return false;
    }

    auto byte = static_cast<unsigned char>(value[cursor++]);
    bool encodedReserved = false;
    if (byte == '%' && cursor + 1 < value.size()) {
        const auto high = decodeHexNibble(value[cursor]);
        const auto low = decodeHexNibble(value[cursor + 1]);
        if (high >= 0 && low >= 0) {
            byte = static_cast<unsigned char>((high << 4) | low);
            cursor += 2;
            // RFC 3986 sections 2.2, 2.3, and 6.2.2.2: percent-encoded
            // unreserved octets are equivalent to their decoded spelling,
            // but an encoded reserved octet is not equivalent to the raw
            // reserved character. Preserve that distinction while still
            // normalizing the case of the hexadecimal spelling.
            encodedReserved = !isUnreservedByte(byte);
        }
    }
    if (!encodedReserved && byte >= 'A' && byte <= 'Z') {
        byte = static_cast<unsigned char>(byte + ('a' - 'A'));
    }
    output = NormalizedHostUnit{
        .byte = byte,
        .encodedReserved = encodedReserved};
    return true;
}

[[nodiscard]] bool isValidConnectAuthorityForm(std::string_view target) noexcept {
    const auto authority = parseHttpAuthority(target);
    if (!authority || authority->portKind() != HttpAuthorityPortKind::kValue) {
        return false;
    }
    // RFC 9110 section 9.3.6 requires a non-empty, valid tunnel destination
    // port. Port zero is reserved and cannot identify that destination.
    return *authority->port() != 0;
}

}  // namespace

struct HttpAuthorityViewAccess final {
    [[nodiscard]] static constexpr HttpAuthorityView make(
        std::string_view host,
        HttpAuthorityPortKind portKind,
        std::uint16_t port = 0) noexcept {
        return HttpAuthorityView(host, portKind, port);
    }
};

bool isValidRequestTargetBytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto byte = static_cast<unsigned char>(target[i]);
        if (byte == '%') {
            if (i + 2 >= target.size() ||
                decodeHexNibble(target[i + 1]) < 0 ||
                decodeHexNibble(target[i + 2]) < 0) {
                return false;
            }
            i += 2;
            continue;
        }
        // RFC 3986 URI-reference is ASCII and consists only of unreserved or
        // reserved characters. A fragment delimiter is never part of an HTTP
        // request target. Brackets are admitted here because this low-level
        // union also covers an IP-literal authority; component validation below
        // rejects them from path and query.
        if (!isUriPchar(byte) && byte != '/' && byte != '?' &&
            byte != '[' && byte != ']') {
            return false;
        }
    }
    return true;
}

bool isValidOriginFormTarget(std::string_view target) noexcept {
    if (target == "*") {
        return true;
    }
    if (target.empty() || target.front() != '/') {
        return false;
    }
    const auto separator = target.find('?');
    const auto path = separator == std::string_view::npos
        ? target
        : target.substr(0, separator);
    const auto query = separator == std::string_view::npos
        ? std::string_view{}
        : target.substr(separator + 1);
    return isValidUriComponent(path, true, false) &&
        isValidUriComponent(query, true, true);
}

bool isValidHttpHost(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        if (value.size() < 3 || value.back() != ']') {
            return false;
        }
        const auto literal = value.substr(1, value.size() - 2);
        return isValidIpv6Literal(literal) || isValidIpvFuture(literal);
    }
    return value.find(':') == std::string_view::npos && isValidRegName(value);
}

std::optional<HttpAuthorityView> parseHttpAuthority(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view portText;
    bool hasPortDelimiter = false;
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = value.substr(0, close + 1);
        const auto remainder = value.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return std::nullopt;
            }
            hasPortDelimiter = true;
            portText = remainder.substr(1);
        }
    } else {
        const auto colon = value.find(':');
        host = colon == std::string_view::npos ? value : value.substr(0, colon);
        if (colon != std::string_view::npos) {
            hasPortDelimiter = true;
            portText = value.substr(colon + 1);
        }
    }

    if (!isValidHttpHost(host)) {
        return std::nullopt;
    }
    if (!hasPortDelimiter) {
        return HttpAuthorityViewAccess::make(
            host, HttpAuthorityPortKind::kAbsent);
    }
    if (portText.empty()) {
        return HttpAuthorityViewAccess::make(
            host, HttpAuthorityPortKind::kEmpty);
    }

    std::uint16_t port = 0;
    if (!parsePortValue(portText, port)) {
        return std::nullopt;
    }
    return HttpAuthorityViewAccess::make(
        host, HttpAuthorityPortKind::kValue, port);
}

bool httpUriHostEquals(std::string_view left, std::string_view right) noexcept {
    std::size_t leftCursor = 0;
    std::size_t rightCursor = 0;
    for (;;) {
        NormalizedHostUnit leftUnit;
        NormalizedHostUnit rightUnit;
        const auto hasLeft = nextNormalizedHostUnit(left, leftCursor, leftUnit);
        const auto hasRight = nextNormalizedHostUnit(right, rightCursor, rightUnit);
        if (!hasLeft || !hasRight) {
            return hasLeft == hasRight;
        }
        if (leftUnit.byte != rightUnit.byte ||
            leftUnit.encodedReserved != rightUnit.encodedReserved) {
            return false;
        }
    }
}

bool isValidHostHeader(std::string_view value) noexcept {
    return parseHttpAuthority(value).has_value();
}

namespace {

[[nodiscard]] bool parseAbsoluteTarget(std::string_view target, RequestTargetView& output) noexcept {
    std::size_t authorityBegin = 0;
    if (target.size() >= 7 && httpAsciiEqualsIgnoreCase(target.substr(0, 7), "http://")) {
        authorityBegin = 7;
        output.defaultPort = 80;
    } else if (target.size() >= 8 && httpAsciiEqualsIgnoreCase(target.substr(0, 8), "https://")) {
        authorityBegin = 8;
        output.defaultPort = 443;
    } else {
        return false;
    }

    const auto rest = target.substr(authorityBegin);
    const auto separator = rest.find_first_of("/?");
    const auto authority = separator == std::string_view::npos ? rest : rest.substr(0, separator);
    if (!isValidHostHeader(authority)) {
        return false;
    }

    output.authority = authority;
    if (separator == std::string_view::npos) {
        output.path = "/";
        output.query = {};
        return true;
    }

    const auto pathBegin = authorityBegin + separator;
    if (target[pathBegin] == '?') {
        const auto query = target.substr(pathBegin + 1);
        if (!isValidUriComponent(query, true, true)) {
            return false;
        }
        output.path = "/";
        output.query = query;
        return true;
    }

    const auto querySeparator = target.find('?', pathBegin);
    const auto path = querySeparator == std::string_view::npos
        ? target.substr(pathBegin)
        : target.substr(pathBegin, querySeparator - pathBegin);
    const auto query = querySeparator == std::string_view::npos
        ? std::string_view{}
        : target.substr(querySeparator + 1);
    if (path.empty() || path.front() != '/' ||
        !isValidUriComponent(path, true, false) ||
        !isValidUriComponent(query, true, true)) {
        return false;
    }
    output.path = path;
    output.query = query;
    return true;
}

}  // namespace

bool authorityMatchesHost(
    std::string_view authority,
    std::string_view host,
    std::uint16_t defaultPort) noexcept {
    const auto authorityParts = parseHttpAuthority(authority);
    const auto hostParts = parseHttpAuthority(host);
    if (!authorityParts || !hostParts ||
        !httpUriHostEquals(authorityParts->host(), hostParts->host())) {
        return false;
    }
    return authorityParts->effectivePort(defaultPort) ==
        hostParts->effectivePort(defaultPort);
}

bool parseRequestTarget(
    HttpKnownMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept {
    if (target == "*") {
        if (method != HttpKnownMethod::kOptions) {
            return false;
        }
        output.path = "*";
        output.query = {};
        output.authority = {};
        output.defaultPort = 0;
        return true;
    }
    if (method == HttpKnownMethod::kConnect) {
        if (!isValidConnectAuthorityForm(target)) {
            return false;
        }
        output.path = target;
        output.query = {};
        output.authority = target;
        output.defaultPort = 0;
        return true;
    }
    if (target.empty()) {
        return false;
    }
    if (target.front() == '/') {
        if (!isValidOriginFormTarget(target)) {
            return false;
        }
        const auto querySeparator = target.find('?');
        output.path = querySeparator == std::string_view::npos
            ? target
            : target.substr(0, querySeparator);
        output.query = querySeparator == std::string_view::npos
            ? std::string_view{}
            : target.substr(querySeparator + 1);
        output.authority = {};
        output.defaultPort = 0;
        return !output.path.empty();
    }
    if (!isValidRequestTargetBytes(target)) {
        return false;
    }
    return parseAbsoluteTarget(target, output);
}

}  // namespace ruvia::detail

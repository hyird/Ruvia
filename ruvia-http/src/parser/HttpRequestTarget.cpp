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
    for (const auto byte : value) {
        if (!isDecimalDigit(byte)) {
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

[[nodiscard]] bool isLowerAlpha(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool isLowerAlphaNumeric(char value) noexcept {
    return isLowerAlpha(value) || isDecimalDigit(value);
}

[[nodiscard]] bool isLowerHexDigit(char value) noexcept {
    return isDecimalDigit(value) || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool isValidSerializedOriginScheme(
    std::string_view scheme) noexcept {
    if (scheme.empty() || !isLowerAlpha(scheme.front())) {
        return false;
    }
    for (const auto value : scheme.substr(1)) {
        if (!isLowerAlphaNumeric(value) && value != '+' && value != '-' &&
            value != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isValidSerializedOriginDomain(
    std::string_view domain) noexcept {
    if (domain.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < domain.size()) {
        const auto separator = domain.find('.', offset);
        const auto label = domain.substr(
            offset,
            separator == std::string_view::npos
                ? std::string_view::npos
                : separator - offset);
        if (label.empty() || !isLowerAlphaNumeric(label.front()) ||
            !isLowerAlphaNumeric(label.back())) {
            return false;
        }
        for (const auto value : label) {
            if (!isLowerAlphaNumeric(value) && value != '-') {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
    return false;
}

[[nodiscard]] bool isValidSerializedOriginH16(
    std::string_view group) noexcept {
    if (group.empty() || group.size() > 4 ||
        (group.size() > 1 && group.front() == '0')) {
        return false;
    }
    for (const auto value : group) {
        if (!isLowerHexDigit(value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool countSerializedOriginIpv6Groups(
    std::string_view side,
    std::size_t& count) noexcept {
    count = 0;
    if (side.empty()) {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = side.find(':', offset);
        const auto group = side.substr(
            offset,
            separator == std::string_view::npos
                ? std::string_view::npos
                : separator - offset);
        if (!isValidSerializedOriginH16(group)) {
            return false;
        }
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

[[nodiscard]] bool isValidSerializedOriginIpv6(
    std::string_view literal) noexcept {
    const auto compression = literal.find("::");
    if (compression == std::string_view::npos) {
        std::size_t groups = 0;
        return countSerializedOriginIpv6Groups(literal, groups) &&
            groups == 8;
    }
    if (literal.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    std::size_t leftGroups = 0;
    std::size_t rightGroups = 0;
    return countSerializedOriginIpv6Groups(
               literal.substr(0, compression),
               leftGroups) &&
        countSerializedOriginIpv6Groups(
            literal.substr(compression + 2),
            rightGroups) &&
        leftGroups + rightGroups <= 6;
}

[[nodiscard]] bool parseSerializedOriginPort(
    std::string_view value,
    std::uint16_t& port) noexcept {
    // A serialized URL port is the shortest decimal form of the URL record's
    // 16-bit port. Merely accepting five digits admits values such as 99999,
    // while accepting leading zeroes admits spellings no serializer can emit.
    if (value.empty() ||
        (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    return parsePortValue(value, port);
}

[[nodiscard]] std::optional<std::uint16_t>
serializedOriginDefaultPort(std::string_view scheme) noexcept {
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

bool isValidOriginOrAsteriskFormTarget(std::string_view target) noexcept {
    return target == "*" || isValidOriginFormTarget(target);
}

bool isValidOriginOrAsteriskFormTarget(
    HttpKnownMethod method,
    std::string_view target) noexcept {
    return target == "*"
        ? method == HttpKnownMethod::kOptions
        : isValidOriginFormTarget(target);
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

bool isValidHttpSerializedOrigin(std::string_view value) noexcept {
    const auto schemeEnd = value.find("://");
    const auto scheme = value.substr(0, schemeEnd);
    if (schemeEnd == std::string_view::npos ||
        !isValidSerializedOriginScheme(scheme)) {
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
            if (authority.find(':', portSeparator + 1) !=
                std::string_view::npos) {
                return false;
            }
            host = authority.substr(0, portSeparator);
            hasPort = true;
            port = authority.substr(portSeparator + 1);
        }
        if (!parseIpv4Address(host) &&
            !isValidSerializedOriginDomain(host)) {
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
    // RFC 9112 section 3.2 requires an empty Host field when the target URI
    // has no authority component. Keep that wire-valid state distinct from a
    // usable network authority: parseHttpAuthority() intentionally continues
    // to require a host.
    return value.empty() || parseHttpAuthority(value).has_value();
}

bool isValidUriAuthority(std::string_view value) noexcept {
    auto hostAndPort = value;
    if (const auto delimiter = value.find('@');
        delimiter != std::string_view::npos) {
        if (!isValidUriUserinfo(value.substr(0, delimiter)) ||
            value.find('@', delimiter + 1) != std::string_view::npos) {
            return false;
        }
        hostAndPort = value.substr(delimiter + 1);
    }

    std::string_view host;
    std::string_view port;
    bool hasPort = false;
    if (!hostAndPort.empty() && hostAndPort.front() == '[') {
        const auto close = hostAndPort.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = hostAndPort.substr(0, close + 1);
        const auto remainder = hostAndPort.substr(close + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') {
                return false;
            }
            hasPort = true;
            port = remainder.substr(1);
        }
    } else {
        const auto delimiter = hostAndPort.find(':');
        host = delimiter == std::string_view::npos
            ? hostAndPort
            : hostAndPort.substr(0, delimiter);
        if (delimiter != std::string_view::npos) {
            hasPort = true;
            port = hostAndPort.substr(delimiter + 1);
            if (port.find(':') != std::string_view::npos) {
                return false;
            }
        }
    }

    return (host.empty() || isValidHttpHost(host)) &&
        (!hasPort || isValidUriPort(port));
}

bool isValidUriScheme(std::string_view value) noexcept {
    const auto isAlpha = [](unsigned char byte) noexcept {
        return (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z');
    };
    if (value.empty() || !isAlpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const auto c : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(c);
        if (!isAlpha(byte) && !(byte >= '0' && byte <= '9') &&
            byte != '+' && byte != '-' && byte != '.') {
            return false;
        }
    }
    return true;
}

std::uint16_t httpUriSchemeDefaultPort(std::string_view scheme) noexcept {
    if (httpAsciiEqualsIgnoreCase(scheme, "http")) {
        return 80;
    }
    if (httpAsciiEqualsIgnoreCase(scheme, "https")) {
        return 443;
    }
    return 0;
}

namespace {

[[nodiscard]] bool parseAbsoluteTarget(
    HttpKnownMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept {
    // RFC 9112 section 3.2.2 defines absolute-form as the complete RFC 3986
    // absolute-URI grammar. Restricting this to HTTP(S) rejects valid proxy
    // requests such as ftp:// targets and every authority-less scheme.
    const auto schemeEnd = target.find(':');
    if (schemeEnd == std::string_view::npos ||
        !isValidUriScheme(target.substr(0, schemeEnd))) {
        return false;
    }

    const auto scheme = target.substr(0, schemeEnd);
    const bool httpScheme = httpAsciiEqualsIgnoreCase(scheme, "http") ||
        httpAsciiEqualsIgnoreCase(scheme, "https");
    const auto remainder = target.substr(schemeEnd + 1);
    const auto querySeparator = remainder.find('?');
    const auto hierarchy = querySeparator == std::string_view::npos
        ? remainder
        : remainder.substr(0, querySeparator);
    const auto query = querySeparator == std::string_view::npos
        ? std::string_view{}
        : remainder.substr(querySeparator + 1);
    if (!isValidUriComponent(query, true, true)) {
        return false;
    }

    std::string_view authority;
    std::string_view path;
    if (hierarchy.starts_with("//")) {
        const auto authorityAndPath = hierarchy.substr(2);
        const auto pathSeparator = authorityAndPath.find('/');
        const auto uriAuthority = pathSeparator == std::string_view::npos
            ? authorityAndPath
            : authorityAndPath.substr(0, pathSeparator);
        path = pathSeparator == std::string_view::npos
            ? std::string_view{}
            : authorityAndPath.substr(pathSeparator);
        if (!isValidUriAuthority(uriAuthority) ||
            !isValidUriComponent(path, true, false)) {
            return false;
        }

        const auto userinfoDelimiter = uriAuthority.find('@');
        authority = userinfoDelimiter == std::string_view::npos
            ? uriAuthority
            : uriAuthority.substr(userinfoDelimiter + 1);
        // Host is the public, authoritative routing value installed by the
        // HTTP/1 parser. It therefore still has to fit the validated HTTP
        // authority representation even when the URI scheme is generic.
        if (!isValidHostHeader(authority)) {
            return false;
        }

        // HTTP(S) URI syntax has a mandatory, non-empty authority. Userinfo in
        // an HTTP URI is rejected at this trust boundary (RFC 9110 section
        // 4.2.4) instead of being silently stripped into a routing identity.
        if (httpScheme &&
            (authority.empty() ||
             userinfoDelimiter != std::string_view::npos)) {
            return false;
        }
    } else {
        // RFC 9110 defines HTTP(S) URI with "//" authority. Other registered
        // schemes may use path-absolute, path-rootless, or path-empty.
        if (httpScheme || !isValidUriComponent(hierarchy, true, false)) {
            return false;
        }
        path = hierarchy;
    }

    if (path.empty() &&
        method == HttpKnownMethod::kOptions && query.empty()) {
        // RFC 9112 section 3.2.4: a proxy forwarding an absolute-form
        // OPTIONS target with an empty path and no query to the final origin
        // must use asterisk-form. Expose that route semantic directly even
        // when this parser itself is the origin-facing recipient.
        output.path = "*";
    } else if (path.empty() &&
               httpScheme && method != HttpKnownMethod::kOptions) {
        // RFC 9110 section 4.2.3 permits this normalization only for HTTP(S)
        // targets that are not OPTIONS. Generic schemes retain their exact
        // empty path, and an OPTIONS target with a query remains distinct.
        output.path = "/";
    } else {
        output.path = path;
    }
    output.query = query;
    output.authority = authority;
    output.defaultPort = httpUriSchemeDefaultPort(scheme);
    output.form = HttpRequestTargetForm::kAbsolute;
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
    // With no known scheme default, an omitted/empty port has no numeric value.
    // In particular, it must not compare equal to the explicit port `:0` merely
    // because zero is also our "unknown default" sentinel.
    if (defaultPort == 0) {
        const bool authorityHasPort =
            authorityParts->portKind() == HttpAuthorityPortKind::kValue;
        const bool hostHasPort =
            hostParts->portKind() == HttpAuthorityPortKind::kValue;
        if (authorityHasPort != hostHasPort) {
            return false;
        }
        return !authorityHasPort || authorityParts->port() == hostParts->port();
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
        output.form = HttpRequestTargetForm::kAsterisk;
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
        output.form = HttpRequestTargetForm::kAuthority;
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
        output.form = HttpRequestTargetForm::kOrigin;
        return !output.path.empty();
    }
    if (!isValidRequestTargetBytes(target)) {
        return false;
    }
    return parseAbsoluteTarget(method, target, output);
}

}  // namespace ruvia::detail

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

[[nodiscard]] bool parsePort(std::string_view value) noexcept {
    std::uint16_t port = 0;
    return parsePortValue(value, port);
}

[[nodiscard]] bool isDecimalDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool isHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'F') ||
        (c >= 'a' && c <= 'f');
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

[[nodiscard]] bool isValidBracketedHost(std::string_view value) noexcept {
    const auto close = value.find(']');
    if (close == std::string_view::npos || close <= 1) {
        return false;
    }
    if (value.find('[', close + 1) != std::string_view::npos) {
        return false;
    }
    if (!isValidIpv6Literal(value.substr(1, close - 1))) {
        return false;
    }
    if (close + 1 == value.size()) {
        return true;
    }
    return value[close + 1] == ':' && parsePort(value.substr(close + 2));
}

struct AuthorityParts {
    std::string_view host;
    std::uint16_t port{0};
    bool hasPort{false};
};

[[nodiscard]] bool splitAuthority(std::string_view value, AuthorityParts& parts) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        parts.host = value.substr(0, close + 1);
        if (close + 1 == value.size()) {
            parts.port = 0;
            parts.hasPort = false;
            return true;
        }
        if (value[close + 1] != ':' || !parsePortValue(value.substr(close + 2), parts.port)) {
            return false;
        }
        parts.hasPort = true;
        return true;
    }

    const auto colon = value.find(':');
    if (colon == std::string_view::npos) {
        parts.host = value;
        parts.port = 0;
        parts.hasPort = false;
        return true;
    }
    parts.host = value.substr(0, colon);
    if (!parsePortValue(value.substr(colon + 1), parts.port)) {
        return false;
    }
    parts.hasPort = true;
    return true;
}

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
        output.path = "/";
        output.query = target.substr(pathBegin + 1);
        return true;
    }

    const auto querySeparator = target.find('?', pathBegin);
    output.path = querySeparator == std::string_view::npos
        ? target.substr(pathBegin)
        : target.substr(pathBegin, querySeparator - pathBegin);
    output.query = querySeparator == std::string_view::npos
        ? std::string_view{}
        : target.substr(querySeparator + 1);
    return !output.path.empty() && output.path.front() == '/';
}

[[nodiscard]] bool isValidConnectAuthorityForm(std::string_view target) noexcept {
    if (!isValidHostHeader(target)) {
        return false;
    }
    if (target.front() == '[') {
        const auto close = target.find(']');
        return close != std::string_view::npos &&
            close + 2 < target.size() &&
            target[close + 1] == ':';
    }
    return target.find(':') != std::string_view::npos;
}

}  // namespace

bool isValidHostHeader(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[') {
        return isValidBracketedHost(value);
    }

    // Single pass over the reg-name: '[' / ']' / a second ':' are not
    // reg-name chars and fall out of the table check; parsePort rejects an
    // empty port and any non-digit remainder (including extra colons).
    std::size_t i = 0;
    for (; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == ':') {
            break;
        }
        if (c == '%') {
            if (i + 2 >= value.size() ||
                decodeHexNibble(value[i + 1]) < 0 ||
                decodeHexNibble(value[i + 2]) < 0) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!kRegNameCharTable[c]) {
            return false;
        }
    }
    if (i == 0) {
        return false;
    }
    if (i == value.size()) {
        return true;
    }
    return parsePort(value.substr(i + 1));
}

bool authorityMatchesHost(
    std::string_view authority,
    std::string_view host,
    std::uint16_t defaultPort) noexcept {
    if (httpAsciiEqualsIgnoreCase(authority, host)) {
        return true;
    }

    AuthorityParts authorityParts;
    AuthorityParts hostParts;
    if (!splitAuthority(authority, authorityParts) || !splitAuthority(host, hostParts)) {
        return false;
    }
    if (!httpAsciiEqualsIgnoreCase(authorityParts.host, hostParts.host)) {
        return false;
    }

    const auto authorityPort = authorityParts.hasPort ? authorityParts.port : defaultPort;
    const auto hostPort = hostParts.hasPort ? hostParts.port : defaultPort;
    return authorityPort == hostPort;
}

bool parseRequestTarget(
    HttpMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept {
    if (target == "*") {
        if (method != HttpMethod::kOptions) {
            return false;
        }
        output.path = "*";
        output.query = {};
        output.authority = {};
        output.defaultPort = 0;
        return true;
    }
    if (method == HttpMethod::kConnect) {
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

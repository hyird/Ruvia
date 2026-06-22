#include "HttpRequestTarget.h"

#include "HttpParserSyntax.h"
#include "../HeaderTokenUtils.h"

#include <array>
#include <charconv>
#include <system_error>

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isValidRequestTargetBytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (const auto ch : target) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F || c == '#') {
            return false;
        }
    }
    return true;
}

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

[[nodiscard]] bool isValidBracketedHost(std::string_view value) noexcept {
    const auto close = value.find(']');
    if (close == std::string_view::npos || close <= 1) {
        return false;
    }
    // RFC 3986 section 3.2.2: inside IP-literal brackets only IPv6/IPv4-mapped hex
    // digits, colons, and dots are valid. Allowing the full reg-name set
    // (which includes sub-delims like '!', '$', etc.) is too permissive.
    for (std::size_t i = 1; i < close; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == ':' || c == '.') {
            continue;
        }
        if (!isHttpHexDigit(c)) {
            return false;
        }
    }
    if (value.find('[', close + 1) != std::string_view::npos) {
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
                !isHttpHexDigit(static_cast<unsigned char>(value[i + 1])) ||
                !isHttpHexDigit(static_cast<unsigned char>(value[i + 2]))) {
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
    if (!isValidRequestTargetBytes(target)) {
        return false;
    }
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
    if (target.front() == '/') {
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
    return parseAbsoluteTarget(target, output);
}

}  // namespace ruvia::detail

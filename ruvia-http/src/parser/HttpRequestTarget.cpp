#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include "ruvia/http/detail/parser/HttpUriGrammar.h"

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace ruvia::detail {
namespace {

// URI reg-name chars: unreserved / sub-delims (pct-encoded handled by caller).



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

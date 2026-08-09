#include "ruvia/web/Context.h"

#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/util/Hex.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"

// A redirect target the application supplies may contain bytes that are not
// legal in a URI-reference carried by Location. Percent-encode exactly those,
// leaving an already valid URI -- including its existing percent-escapes -- byte
// for byte intact, so a caller that encoded correctly is never double-encoded.

namespace ruvia {
namespace {

struct RedirectAuthoritySpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool present = false;
};

[[nodiscard]] bool isValidPercentEscape(std::string_view value, std::size_t index) noexcept {
    return index + 2 < value.size() && detail::decodeHexNibble(value[index + 1]) >= 0 && detail::decodeHexNibble(value[index + 2]) >= 0;
}

[[nodiscard]] bool isUriSchemeFirst(unsigned char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

[[nodiscard]] bool isUriSchemeRest(unsigned char ch) noexcept {
    return isUriSchemeFirst(ch) || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
}

[[nodiscard]] std::size_t findRedirectAuthorityEnd(std::string_view location, std::size_t begin) noexcept {
    for (std::size_t i = begin; i < location.size(); ++i) {
        switch (location[i]) {
            case '/':
            case '?':
            case '#':
                return i;
            default:
                break;
        }
    }
    return location.size();
}

[[nodiscard]] RedirectAuthoritySpan findRedirectAuthority(std::string_view location) noexcept {
    if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
        return RedirectAuthoritySpan{2, findRedirectAuthorityEnd(location, 2), true};
    }
    if (location.empty() || !isUriSchemeFirst(static_cast<unsigned char>(location.front()))) {
        return {};
    }
    for (std::size_t i = 1; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        if (ch == ':') {
            const auto begin = i + 3;
            if (begin <= location.size() && location[i + 1] == '/' && location[i + 2] == '/') {
                return RedirectAuthoritySpan{begin, findRedirectAuthorityEnd(location, begin), true};
            }
            return {};
        }
        if (ch == '/' || ch == '?' || ch == '#' || !isUriSchemeRest(ch)) {
            return {};
        }
    }
    return {};
}

[[nodiscard]] bool isIpLiteralBracketDelimiter(std::string_view location, std::size_t index, RedirectAuthoritySpan authority) noexcept {
    if (!authority.present || index < authority.begin || index >= authority.end) {
        return false;
    }

    std::size_t hostBegin = authority.begin;
    for (std::size_t i = authority.begin; i < authority.end; ++i) {
        if (location[i] == '@') {
            hostBegin = i + 1;
        }
    }
    if (hostBegin >= authority.end || location[hostBegin] != '[') {
        return false;
    }

    for (std::size_t i = hostBegin + 1; i < authority.end; ++i) {
        if (location[i] == ']') {
            return index == hostBegin || index == i;
        }
    }
    return false;
}

[[nodiscard]] bool redirectLocationContainsLineBreak(std::string_view location) noexcept {
    for (const char ch : location) {
        if (ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool encodeUriKeepsByte(unsigned char ch, std::string_view location, std::size_t index, RedirectAuthoritySpan authority) noexcept;

[[nodiscard]] bool redirectLocationNeedsEncoding(std::string_view location) noexcept {
    const auto authority = findRedirectAuthority(location);
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        if (ch == '%') {
            if (!isValidPercentEscape(location, i)) {
                return true;
            }
            i += 2;
            continue;
        }
        if (!encodeUriKeepsByte(ch, location, i, authority)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool encodeUriKeepsByte(unsigned char ch, std::string_view location, std::size_t index, RedirectAuthoritySpan authority) noexcept {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return true;
    }

    if (ch == '[' || ch == ']') {
        return isIpLiteralBracketDelimiter(location, index, authority);
    }

    switch (ch) {
        case ';':
        case ',':
        case '/':
        case '?':
        case ':':
        case '@':
        case '&':
        case '=':
        case '+':
        case '$':
        case '-':
        case '_':
        case '.':
        case '!':
        case '~':
        case '*':
        case '\'':
        case '(':
        case ')':
        case '#':
            return true;
        default:
            return false;
    }
}

void appendPercentEncodedByte(std::pmr::string& output, unsigned char ch) {
    output.push_back('%');
    output.push_back(detail::upperHexDigit(ch >> 4));
    output.push_back(detail::upperHexDigit(ch & 0x0F));
}

[[nodiscard]] std::pmr::string encodeRedirectLocation(std::string_view location, std::pmr::memory_resource* resource) {
    std::pmr::string encoded(resource);
    encoded.reserve(location.size());
    const auto authority = findRedirectAuthority(location);
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        // Pass an already well-formed percent-escape (%HH) through verbatim. The
        // encoder rewrites the whole string when any byte needs escaping -- so
        // without this, a location that is already percent-encoded elsewhere
        // (e.g. "%20") would have its '%' re-encoded to "%25", double-encoding
        // it to "%2520" and corrupting the redirect target.
        // RFC 3986 2.4 forbids encoding the same string more than once; the caller
        // means a valid target, not a literal percent. A lone or malformed '%' is
        // not a valid escape and is percent-encoded like any other octet below.
        if (ch == '%' && isValidPercentEscape(location, i)) {
            encoded.push_back('%');
            encoded.push_back(location[i + 1]);
            encoded.push_back(location[i + 2]);
            i += 2;
            continue;
        }
        if (encodeUriKeepsByte(ch, location, i, authority)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        appendPercentEncodedByte(encoded, ch);
    }
    return encoded;
}

[[nodiscard]] bool isRedirectStatus(HttpStatusCode statusCode) noexcept {
    return statusCode == http_status::kMovedPermanently || statusCode == http_status::kFound || statusCode == http_status::kSeeOther || statusCode == http_status::kTemporaryRedirect || statusCode == http_status::kPermanentRedirect;
}

}  // namespace

HttpResponse Context::redirect(std::string_view location, HttpStatusCode statusCode) const {
    if (!isRedirectStatus(statusCode)) {
        throw std::invalid_argument("redirect status must be 301, 302, 303, 307, or 308");
    }
    if (redirectLocationContainsLineBreak(location)) {
        throw std::invalid_argument("redirect location must not contain CR or LF");
    }
    HttpResponse response(resource());
    applyResponseState(response, statusCode);
    if (redirectLocationNeedsEncoding(location)) {
        auto encodedLocation = encodeRedirectLocation(location, resource());
        response.header("Location", encodedLocation);
    } else {
        response.header("Location", location);
    }
    return response;
}

}  // namespace ruvia

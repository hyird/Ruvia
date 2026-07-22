#include "ruvia/web/Context.h"

#include <string_view>

#include "ruvia/http/detail/Hex.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"

// A redirect target the application supplies may contain bytes that are not
// legal in a Location field. Percent-encode exactly those, leaving an already
// valid URI -- including its existing percent-escapes -- byte for byte intact,
// so a caller that encoded correctly is never double-encoded.

namespace ruvia {
namespace {

[[nodiscard]] bool redirectLocationNeedsEncoding(std::string_view location) noexcept {
    return std::ranges::any_of(location, [](char ch) noexcept {
        return static_cast<unsigned char>(ch) >= 0x80;
    });
}

[[nodiscard]] bool encodeUriKeepsByte(unsigned char ch) noexcept {
    if ((ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9')) {
        return true;
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

[[nodiscard]] std::pmr::string encodeRedirectLocation(
    std::string_view location,
    std::pmr::memory_resource* resource) {
    std::pmr::string encoded(resource);
    encoded.reserve(location.size());
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        // Pass an already well-formed percent-escape (%HH) through verbatim. This
        // pass only runs when the location carries a non-ASCII byte, but it then
        // rewrites the whole string -- so without this, a location that is already
        // percent-encoded elsewhere (e.g. "%20") would have its '%' re-encoded to
        // "%25", double-encoding it to "%2520" and corrupting the redirect target.
        // RFC 3986 2.4 forbids encoding the same string more than once; the caller
        // means a valid target, not a literal percent. A lone or malformed '%' is
        // not a valid escape and is percent-encoded like any other octet below.
        if (ch == '%' && i + 2 < location.size() &&
            detail::decodeHexNibble(location[i + 1]) >= 0 &&
            detail::decodeHexNibble(location[i + 2]) >= 0) {
            encoded.push_back('%');
            encoded.push_back(location[i + 1]);
            encoded.push_back(location[i + 2]);
            i += 2;
            continue;
        }
        if (encodeUriKeepsByte(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        appendPercentEncodedByte(encoded, ch);
    }
    return encoded;
}

}  // namespace

HttpResponse Context::redirect(
    std::string_view location,
    HttpStatusCode statusCode) const {
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

#include <stdexcept>
#include <string_view>

#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"

// A redirect target the application supplies may contain bytes that are not
// legal in a URI-reference carried by Location; ruvia-http percent-encodes
// exactly those and leaves an already valid URI byte for byte intact.

namespace ruvia {
namespace {

[[nodiscard]] bool redirect_location_contains_line_break(std::string_view location) noexcept {
    for (const char ch : location) {
        if (ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_redirect_status(http_status_code status_code) noexcept {
    return status_code == http_status::moved_permanently || status_code == http_status::found ||
           status_code == http_status::see_other || status_code == http_status::temporary_redirect ||
           status_code == http_status::permanent_redirect;
}

// RFC 3986 section 3.2 authority of an absolute or network-path reference:
// it follows "//" and ends at the first "/", "?" or "#". Browsers treat "\" as
// a path separator for http(s) (WHATWG URL), so a percent-encoded "\" inside
// the authority would move the host the browser contacts, e.g.
// "http://good.example\@evil.example/".
[[nodiscard]] bool redirect_authority_contains_backslash(std::string_view location) noexcept {
    std::size_t start = 0;
    if (!location.starts_with("//")) {
        const auto colon = location.find(':');
        if (colon == std::string_view::npos || colon == 0 || location.substr(colon + 1, 2) != "//") {
            return false;
        }
        for (std::size_t index = 0; index < colon; ++index) {
            const char ch = location[index];
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!alpha && (index == 0 || !((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'))) {
                return false;
            }
        }
        start = colon + 1;
    }
    const auto authority = location.substr(start + 2);
    return authority.substr(0, authority.find_first_of("/?#")).find('\\') != std::string_view::npos;
}

}  // namespace

http_response context::redirect(redirect_response_options options) const {
    const auto location = options.location_.view();
    const auto status_code = options.status_;
    if (!is_redirect_status(status_code)) {
        throw std::invalid_argument("redirect status must be 301, 302, 303, 307, or 308");
    }
    if (redirect_location_contains_line_break(location)) {
        throw std::invalid_argument("redirect location must not contain CR or LF");
    }
    if (redirect_authority_contains_backslash(location)) {
        throw std::invalid_argument("redirect location authority must not contain a backslash");
    }
    http_response response(arena());
    apply_response_state(response, status_code);
    if (const auto encoded_location = percent_encode_uri_reference(location, pool())) {
        response.header("Location", *encoded_location);
    } else {
        response.header("Location", location);
    }
    return response;
}

}  // namespace ruvia

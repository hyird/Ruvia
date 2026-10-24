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

#pragma once

#include <string_view>

#include "ruvia/http/http_field_whitespace.h"

namespace ruvia {

// Visits literal semicolon-separated Cookie name/value pairs. A quote has no
// special meaning in a Cookie field. Segments without '=' are skipped, names
// and values are OWS-trimmed, and returning false stops traversal. Views borrow
// the input field.
template <typename visitor_type>
inline void http_visit_cookie_pairs(std::string_view value, visitor_type&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = http_trim_ows(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)))) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

}  // namespace ruvia

#pragma once

#include <string_view>

#include "ruvia/http/HttpFieldWhitespace.h"

namespace ruvia {

// Visits literal semicolon-separated Cookie name/value pairs. A quote has no
// special meaning in a Cookie field. Segments without '=' are skipped, names
// and values are OWS-trimmed, and returning false stops traversal. Views borrow
// the input field.
template <typename Visitor>
inline void httpVisitCookiePairs(std::string_view value, Visitor&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = httpTrimOws(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(httpTrimOws(part.substr(0, equals)), httpTrimOws(part.substr(equals + 1)))) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

}  // namespace ruvia

#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/http_field_whitespace.h"

namespace ruvia {
namespace detail {

// Shared delimiter scanner. Quoted-pair bytes are data; protocol-specific
// interpretation of a segment belongs to the composed visitor.
template <char separator, typename visitor_type>
inline void visit_quoted_field_segments(std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        bool quoted = false;
        std::size_t end = start;
        for (; end < value.size(); ++end) {
            const char c = value[end];
            if (quoted) {
                if (c == '\\' && end + 1 < value.size()) {
                    ++end;
                } else if (c == '"') {
                    quoted = false;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == separator) {
                break;
            }
        }
        const auto item = http_trim_ows(value.substr(start, end - start));
        if (!visitor(item) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

}  // namespace detail

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Empty members are reported. Returning
// false from the visitor stops traversal. Views borrow the input field.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(
    std::string_view value, visitor_type&& visitor) {
    detail::visit_quoted_field_segments<','>(value, std::forward<visitor_type>(visitor));
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Views borrow the input.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(
    std::string_view value, visitor_type&& visitor) {
    detail::visit_quoted_field_segments<';'>(value, [&visitor](std::string_view part) {
        const auto equals = part.find('=');
        return equals == std::string_view::npos ||
               visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
    });
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] inline std::string_view http_trim_quoted_field_value(
    std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace ruvia
